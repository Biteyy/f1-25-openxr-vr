#include "MirrorWindow.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#include "XrDriverPrivate.h"
#include "generated/interfaces/vrtypes.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "dxgi.lib")

// The visor drawn over the window's copy. For each window pixel: its place in the eye image, its direction through the
// eye's field of view (tangents), and so its place on the visor's plane, which spans +-span (tangent) across and up.
static const char kVisorShaders[] = R"(
cbuffer Constants : register(b0) {
	float eyeW, eyeH, bandTop, span;
	float tanL, tanR, tanU, tanD;
};
Texture2D visor : register(t0);
SamplerState linearClamp : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
	float u = pos.x / eyeW;
	float v = (pos.y + bandTop) / eyeH;
	float tx = tanL + u * (tanR - tanL);
	float ty = tanU - v * (tanU - tanD);
	return visor.Sample(linearClamp, float2(tx / span * 0.5 + 0.5, 0.5 - ty / span * 0.5));
}
)";

typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* SerializeFn)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

using Microsoft::WRL::ComPtr;

static const wchar_t kClassName[] = L"OpenXrVrMirror";
static const UINT WM_MIRROR_CLOSE = WM_APP + 1; // asks the window thread to close the window (not the player)

MirrorWindow::~MirrorWindow()
{
	Close();
}

LRESULT CALLBACK MirrorWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	auto* self = (MirrorWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	switch (msg) {
	case WM_CLOSE:
		// The player closed it: keep it closed until the setting is switched off and on again
		if (self)
			self->closedByUser = true;
		DestroyWindow(hwnd);
		return 0;
	case WM_MIRROR_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		if (self)
			self->hwnd = nullptr;
		PostQuitMessage(0);
		return 0;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE; // clicking it doesn't take focus from the game
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool MirrorWindow::OpenWindow()
{
	if (thread.joinable())
		thread.join(); // a window the player closed: its thread has ended

	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	thread = std::thread([this, ready] {
		HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc{ sizeof(wc) };
		wc.lpfnWndProc = WindowProc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW (the project builds without UNICODE)
		wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		wc.lpszClassName = kClassName;
		RegisterClassExW(&wc); // fails harmlessly when already registered

		RECT rect{ 0, 0, 1280, 720 };
		AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
		HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_APPWINDOW, kClassName, L"F1 25 - VR view", WS_OVERLAPPEDWINDOW,
		    CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
		if (window) {
			SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)this);
			ShowWindow(window, SW_SHOWNOACTIVATE);
		}
		threadId = GetCurrentThreadId();
		hwnd = window;
		SetEvent(ready);
		if (!window)
			return;

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	});
	WaitForSingleObject(ready, 5000);
	CloseHandle(ready);

	if (!hwnd) {
		OOVR_LOG("VR view window: can't create the window");
		return false;
	}
	OOVR_LOG("VR view window: opened");
	return true;
}

void MirrorWindow::ReleaseChain()
{
	if (fence && queue) {
		// Let the copies in flight finish before their resources go
		queue->Signal(fence.Get(), nextFenceValue);
		if (fence->GetCompletedValue() < nextFenceValue) {
			HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(nextFenceValue, done);
			WaitForSingleObject(done, 1000);
			CloseHandle(done);
		}
		nextFenceValue++;
	}
	if (frameWait) {
		CloseHandle(frameWait);
		frameWait = nullptr;
	}
	chain.Reset();
	list.Reset();
	for (auto& a : allocators)
		a.Reset();
	fence.Reset();
	queue.Reset();
	visorPipeline.Reset();
	visorRoot.Reset();
	visorSrvHeap.Reset();
	rtvHeap.Reset();
	visorTexture.Reset();
	visorGeneration = ~0u;
	eyeWidth = 0;
	eyeHeight = 0;
	eyeFormat = DXGI_FORMAT_UNKNOWN;
}

void MirrorWindow::Close()
{
	ReleaseChain();
	if (thread.joinable()) {
		if (HWND window = hwnd.load())
			PostMessageW(window, WM_MIRROR_CLOSE, 0, 0);
		thread.join();
	}
	hwnd = nullptr;
	closedByUser = false;
	failed = false;
	visorBroken = false;
}

bool MirrorWindow::MakeVisorPipeline(DXGI_FORMAT format)
{
	ComPtr<ID3D12Device> device;
	if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
		return false;
	auto fail = [this](const char* what, HRESULT hr) {
		OOVR_LOGF("VR view window: %s failed (0x%08x), so it shows no visor", what, (unsigned)hr);
		visorBroken = true;
		return false;
	};
	HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	CompileFn compile = compiler ? (CompileFn)GetProcAddress(compiler, "D3DCompile") : nullptr;
	HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
	SerializeFn serialize = d3d12 ? (SerializeFn)GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr;
	if (!compile || !serialize)
		return fail("finding the shader compiler", E_FAIL);
	ComPtr<ID3DBlob> vs, ps, errors, rs;
	HRESULT hr = compile(kVisorShaders, sizeof(kVisorShaders) - 1, "visor", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors);
	if (SUCCEEDED(hr))
		hr = compile(kVisorShaders, sizeof(kVisorShaders) - 1, "visor", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors);
	if (FAILED(hr))
		return fail("compiling the visor shaders", hr);

	D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants = { 0, 0, 8 };
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable = { 1, &range };
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rsDesc{ 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	if (FAILED(hr = serialize(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &errors)) || FAILED(hr = device->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&visorRoot))))
		return fail("making the visor root signature", hr);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = visorRoot.Get();
	desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
	blend.BlendEnable = TRUE;
	blend.SrcBlend = D3D12_BLEND_SRC_ALPHA; // the visor image has straight alpha
	blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOp = D3D12_BLEND_OP_ADD;
	blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
	blend.DestBlendAlpha = D3D12_BLEND_ONE;
	blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = format;
	desc.SampleDesc.Count = 1;
	if (FAILED(hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&visorPipeline))))
		return fail("making the visor pipeline", hr);

	D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
	if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&visorSrvHeap))))
		return fail("CreateDescriptorHeap", hr);
	heap = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
	if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap))))
		return fail("CreateDescriptorHeap", hr);
	return true;
}

bool MirrorWindow::UploadVisor(const Visor& visor)
{
	ComPtr<ID3D12Device> device;
	if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
		return false;
	HRESULT hr;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = visor.width;
	desc.Height = visor.height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // the window's buffer holds the game's (already encoded) colours
	desc.SampleDesc.Count = 1;
	D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT }, uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
	ComPtr<ID3D12Resource> texture, upload;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> copy;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
	UINT64 total = 0;
	device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
	D3D12_RESOURCE_DESC buf{};
	buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf.Width = total;
	buf.Height = buf.DepthOrArraySize = buf.MipLevels = 1;
	buf.SampleDesc.Count = 1;
	buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	unsigned char* mapped = nullptr;
	if (FAILED(hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)))
	    || FAILED(hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))
	    || FAILED(hr = upload->Map(0, nullptr, (void**)&mapped))
	    || FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
	    || FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&copy)))) {
		OOVR_LOGF("VR view window: uploading the visor failed (0x%08x), so it shows no visor", (unsigned)hr);
		visorBroken = true;
		return false;
	}
	for (unsigned y = 0; y < visor.height; y++)
		memcpy(mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch, visor.rgba + (size_t)y * visor.width * 4, (size_t)visor.width * 4);
	upload->Unmap(0, nullptr);
	D3D12_TEXTURE_COPY_LOCATION dst{ texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
	src.PlacedFootprint = fp;
	copy->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = texture.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	copy->ResourceBarrier(1, &barrier);
	copy->Close();
	ID3D12CommandList* lists[] = { copy.Get() };
	queue->ExecuteCommandLists(1, lists);
	// Once per visor change: wait for the copy, so the upload memory can go
	UINT64 value = nextFenceValue++;
	queue->Signal(fence.Get(), value);
	if (fence->GetCompletedValue() < value) {
		HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		fence->SetEventOnCompletion(value, done);
		WaitForSingleObject(done, 2000);
		CloseHandle(done);
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(texture.Get(), &srv, visorSrvHeap->GetCPUDescriptorHandleForHeapStart());
	visorTexture = texture;
	visorGeneration = visor.generation;
	return true;
}

// The swapchain format that the eye texture can be copied into as it is: the same family, displayable
static DXGI_FORMAT ChainFormatFor(DXGI_FORMAT eye)
{
	switch (eye) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

bool MirrorWindow::CreateChain(ID3D12CommandQueue* gameQueue, const D3D12_RESOURCE_DESC& eye)
{
	ReleaseChain();

	DXGI_FORMAT format = ChainFormatFor(eye.Format);
	if (format == DXGI_FORMAT_UNKNOWN) {
		OOVR_LOGF("VR view window: the game's eye format %d can't be shown, so it stays empty", eye.Format);
		return false;
	}

	// The middle 16:9 band of the eye image (all of it if it's wider than that)
	UINT width = (UINT)eye.Width;
	UINT height = (std::min)(eye.Height, width * 9 / 16);
	band = { 0, (eye.Height - height) / 2, 0, width, (eye.Height - height) / 2 + height, 1 };

	ComPtr<ID3D12Device> device;
	ComPtr<IDXGIFactory2> factory;
	HRESULT hr;
	const char* step = "";
	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = width;
	desc.Height = height;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = kBuffers;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	ComPtr<IDXGISwapChain1> chain1;

	if (FAILED(hr = gameQueue->GetDevice(IID_PPV_ARGS(&device))))
		step = "GetDevice";
	else if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
		step = "CreateDXGIFactory2";
	else if (FAILED(hr = factory->CreateSwapChainForHwnd(gameQueue, hwnd.load(), &desc, nullptr, nullptr, &chain1)))
		step = "CreateSwapChainForHwnd";
	else if (FAILED(hr = chain1.As(&chain)))
		step = "IDXGISwapChain3";
	else if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		step = "CreateFence";
	else {
		factory->MakeWindowAssociation(hwnd.load(), DXGI_MWA_NO_ALT_ENTER);
		chain->SetMaximumFrameLatency(1);
		frameWait = chain->GetFrameLatencyWaitableObject();
		for (auto& a : allocators) {
			if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) {
				step = "CreateCommandAllocator";
				break;
			}
		}
		if (!*step && FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list))))
			step = "CreateCommandList";
		if (!*step)
			list->Close();
	}
	if (*step) {
		OOVR_LOGF("VR view window: %s failed (0x%08x), so it stays empty", step, (unsigned)hr);
		ReleaseChain();
		return false;
	}

	queue = gameQueue;
	eyeWidth = eye.Width;
	eyeHeight = eye.Height;
	eyeFormat = eye.Format;
	for (auto& v : fenceValues)
		v = 0;
	OOVR_LOGF("VR view window: showing %ux%u of the %llux%u eye image (format %d)", width, height, (unsigned long long)eye.Width, eye.Height, eye.Format);
	return true;
}

void MirrorWindow::Submit(const vr::D3D12TextureData_t* eye, const Visor& visor)
{
	if (failed || closedByUser || !eye || !eye->m_pResource || !eye->m_pCommandQueue)
		return;

	if (!hwnd.load()) {
		ReleaseChain(); // the old window's chain, if the window went
		if (!OpenWindow()) {
			failed = true;
			return;
		}
	}
	HWND window = hwnd.load();
	if (!window || IsIconic(window))
		return; // closing, or minimised: nothing to show

	D3D12_RESOURCE_DESC desc = eye->m_pResource->GetDesc();
	if (!chain || desc.Width != eyeWidth || desc.Height != eyeHeight || desc.Format != eyeFormat || queue.Get() != eye->m_pCommandQueue) {
		if (!CreateChain(eye->m_pCommandQueue, desc)) {
			failed = true;
			return;
		}
	}

	// Skip the frame rather than wait: when the buffer's last copy hasn't finished, or the window can't take a frame
	UINT index = chain->GetCurrentBackBufferIndex();
	if (fence->GetCompletedValue() < fenceValues[index])
		return;
	if (WaitForSingleObject(frameWait, 0) != WAIT_OBJECT_0)
		return;

	ComPtr<ID3D12Resource> buffer;
	if (FAILED(chain->GetBuffer(index, IID_PPV_ARGS(&buffer)))) {
		chain->Present(0, 0); // gives back the frame-latency count just taken
		return;
	}

	// The visor, when there is one: the pipeline is made for the window's format once, the image uploaded on change
	bool drawVisor = visor.rgba && visor.width && !visorBroken;
	if (drawVisor && !visorPipeline)
		drawVisor = MakeVisorPipeline(ChainFormatFor(eyeFormat));
	if (drawVisor && visor.generation != visorGeneration)
		drawVisor = UploadVisor(visor);

	allocators[index]->Reset();
	list->Reset(allocators[index].Get(), nullptr);

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = buffer.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	list->ResourceBarrier(1, &barrier);

	// The eye texture is read as the headset copy reads it (DX12Compositor::CopyToSwapchain): no barrier of its own
	D3D12_TEXTURE_COPY_LOCATION dst{ buffer.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src{ eye->m_pResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	src.SubresourceIndex = 0;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, &band);

	if (drawVisor) {
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		list->ResourceBarrier(1, &barrier);
		ComPtr<ID3D12Device> device;
		queue->GetDevice(IID_PPV_ARGS(&device));
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
		device->CreateRenderTargetView(buffer.Get(), nullptr, rtv);
		list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
		UINT w = band.right - band.left, h = band.bottom - band.top;
		D3D12_VIEWPORT viewport{ 0, 0, (float)w, (float)h, 0, 1 };
		D3D12_RECT scissor{ 0, 0, (LONG)w, (LONG)h };
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &scissor);
		list->SetPipelineState(visorPipeline.Get());
		list->SetGraphicsRootSignature(visorRoot.Get());
		ID3D12DescriptorHeap* heaps[] = { visorSrvHeap.Get() };
		list->SetDescriptorHeaps(1, heaps);
		list->SetGraphicsRootDescriptorTable(1, visorSrvHeap->GetGPUDescriptorHandleForHeapStart());
		float span = std::tan(visor.halfAngleDeg * 3.14159265f / 180.0f);
		float constants[8] = { (float)eyeWidth, (float)eyeHeight, (float)band.top, span, visor.tanLeft, visor.tanRight, visor.tanUp, visor.tanDown };
		list->SetGraphicsRoot32BitConstants(0, 8, constants, 0);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->DrawInstanced(3, 1, 0, 0);
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	} else {
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	list->ResourceBarrier(1, &barrier);
	list->Close();

	ID3D12CommandList* lists[] = { list.Get() };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.Get(), nextFenceValue);
	fenceValues[index] = nextFenceValue++;

	HRESULT hr = chain->Present(0, 0);
	if (FAILED(hr)) {
		OOVR_LOGF("VR view window: Present failed (0x%08x); it reopens on the next frame", (unsigned)hr);
		ReleaseChain();
	}
}

#endif
