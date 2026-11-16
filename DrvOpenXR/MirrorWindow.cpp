#include "MirrorWindow.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#include "XrDriverPrivate.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

// One full-window triangle: the eye band scaled to the window's frame
static const char kShaders[] = R"(
cbuffer Constants : register(b0) {
	float outW, outH, encode, unused;
};
Texture2D eye : register(t0);
SamplerState linearClamp : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position {
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
	float2 uv = pos.xy / float2(outW, outH);
	float3 c = eye.SampleLevel(linearClamp, uv, 0).rgb;
	if (encode > 0.5) {
		c = saturate(c);
		c = lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
	}
	return float4(c, 1);
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
	case WM_PAINT:
		if (self) {
			self->Paint(hwnd);
			return 0;
		}
		break;
	case WM_ERASEBKGND:
		return 1; // Paint covers the whole window
	case WM_SIZE:
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
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

void MirrorWindow::Paint(HWND window)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(window, &ps);
	RECT client;
	GetClientRect(window, &client);
	int cw = client.right, ch = client.bottom;

	// The frame as large as fits, 16:9, black bars around it
	int dw = cw, dh = cw * (int)kHeight / (int)kWidth;
	if (dh > ch) {
		dh = ch;
		dw = ch * (int)kWidth / (int)kHeight;
	}
	int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
	HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RECT bars[4] = { { 0, 0, cw, dy }, { 0, dy + dh, cw, ch }, { 0, dy, dx, dy + dh }, { dx + dw, dy, cw, dy + dh } };
	for (const RECT& r : bars)
		if (r.right > r.left && r.bottom > r.top)
			FillRect(dc, &r, black);

	{
		std::lock_guard<std::mutex> guard(frameMutex);
		if (haveFrame && dw > 0 && dh > 0) {
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = (LONG)kWidth;
			info.bmiHeader.biHeight = -(LONG)kHeight; // top-down
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB; // BGRA bytes, as the readback holds them
			SetStretchBltMode(dc, HALFTONE);
			SetBrushOrgEx(dc, 0, 0, nullptr);
			StretchDIBits(dc, dx, dy, dw, dh, 0, 0, kWidth, kHeight, frame.data(), &info, DIB_RGB_COLORS, SRCCOPY);
		} else {
			RECT r{ dx, dy, dx + dw, dy + dh };
			FillRect(dc, &r, black);
		}
	}
	EndPaint(window, &ps);
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
		wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); // the game's own icon, so share pickers show it
		wc.hbrBackground = nullptr;
		wc.lpszClassName = kClassName;
		RegisterClassExW(&wc); // fails harmlessly when already registered

		RECT rect{ 0, 0, (LONG)kWidth, (LONG)kHeight };
		AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
		// A plain top-level app window (no WS_EX_NOACTIVATE, which hides it from some share pickers); it is shown
		// without activating and WM_MOUSEACTIVATE keeps clicks from taking focus
		HWND window = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"F1 25 - VR view", WS_OVERLAPPEDWINDOW,
		    CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
		if (window) {
			SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)this);
			ShowWindow(window, SW_SHOWNOACTIVATE);
		}
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

void MirrorWindow::ReleaseResources()
{
	if (fence && queue) {
		// Let the frames in flight finish before their resources go
		queue->Signal(fence.Get(), nextFenceValue);
		if (fence->GetCompletedValue() < nextFenceValue) {
			HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(nextFenceValue, done);
			WaitForSingleObject(done, 1000);
			CloseHandle(done);
		}
		nextFenceValue++;
	}
	for (Slot& s : slots)
		s = Slot{};
	list.Reset();
	band.Reset();
	target.Reset();
	pipeline.Reset();
	root.Reset();
	srvHeap.Reset();
	rtvHeap.Reset();
	fence.Reset();
	queue.Reset();
	eyeWidth = 0;
	eyeHeight = 0;
	eyeFormat = DXGI_FORMAT_UNKNOWN;
}

void MirrorWindow::Close()
{
	ReleaseResources();
	if (thread.joinable()) {
		if (HWND window = hwnd.load())
			PostMessageW(window, WM_MIRROR_CLOSE, 0, 0);
		thread.join();
	}
	hwnd = nullptr;
	{
		std::lock_guard<std::mutex> guard(frameMutex);
		haveFrame = false;
	}
	closedByUser = false;
	failed = false;
}

bool MirrorWindow::MakePipeline(ID3D12Device* device)
{
	HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	CompileFn compile = compiler ? (CompileFn)GetProcAddress(compiler, "D3DCompile") : nullptr;
	HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
	SerializeFn serialize = d3d12 ? (SerializeFn)GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr;
	if (!compile || !serialize) {
		OOVR_LOG("VR view window: no shader compiler, so it stays empty");
		return false;
	}
	ComPtr<ID3DBlob> vs, ps, errors, rs;
	HRESULT hr = compile(kShaders, sizeof(kShaders) - 1, "mirror", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors);
	if (SUCCEEDED(hr))
		hr = compile(kShaders, sizeof(kShaders) - 1, "mirror", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors);
	if (FAILED(hr)) {
		OOVR_LOGF("VR view window: compiling the shaders failed (0x%08x): %s", (unsigned)hr, errors ? (const char*)errors->GetBufferPointer() : "");
		return false;
	}

	D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants = { 0, 0, 4 };
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
	if (FAILED(hr = serialize(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &errors)) || FAILED(hr = device->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root)))) {
		OOVR_LOGF("VR view window: making the root signature failed (0x%08x)", (unsigned)hr);
		return false;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = root.Get();
	desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	if (FAILED(hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
		OOVR_LOGF("VR view window: making the pipeline failed (0x%08x)", (unsigned)hr);
		return false;
	}
	return true;
}

// The format the eye texture can be copied into as it is (the same family), and whether it holds linear light
static DXGI_FORMAT BandFormatFor(DXGI_FORMAT eye, bool& linear)
{
	linear = false;
	switch (eye) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_UNORM; // read as the encoded bytes: the frame wants them encoded
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		linear = true;
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

bool MirrorWindow::CreateResources(ID3D12CommandQueue* gameQueue, const D3D12_RESOURCE_DESC& eye)
{
	ReleaseResources();

	bool linear = false;
	DXGI_FORMAT format = BandFormatFor(eye.Format, linear);
	if (format == DXGI_FORMAT_UNKNOWN) {
		OOVR_LOGF("VR view window: the game's eye format %d can't be shown, so it stays empty", eye.Format);
		return false;
	}
	encodeSrgb = linear;

	// The middle 16:9 band of the eye image (all of it if it's wider than that)
	UINT width = (UINT)eye.Width;
	UINT height = (std::min)(eye.Height, width * 9 / 16);
	bandBox = { 0, (eye.Height - height) / 2, 0, width, (eye.Height - height) / 2 + height, 1 };

	ComPtr<ID3D12Device> device;
	HRESULT hr = S_OK;
	const char* step = "";
	D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT }, readbackHeap{ D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC tex{};
	tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex.DepthOrArraySize = 1;
	tex.MipLevels = 1;
	tex.SampleDesc.Count = 1;
	D3D12_RESOURCE_DESC buf{};
	buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf.Width = (UINT64)kRowPitch * kHeight;
	buf.Height = buf.DepthOrArraySize = buf.MipLevels = 1;
	buf.SampleDesc.Count = 1;
	buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	if (FAILED(hr = gameQueue->GetDevice(IID_PPV_ARGS(&device))))
		step = "GetDevice";
	else if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		step = "CreateFence";
	if (!*step) {
		tex.Width = width;
		tex.Height = height;
		tex.Format = format;
		if (FAILED(hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&band))))
			step = "creating the band texture";
	}
	if (!*step) {
		tex.Width = kWidth;
		tex.Height = kHeight;
		tex.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (FAILED(hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target))))
			step = "creating the frame texture";
	}
	for (Slot& s : slots) {
		if (*step)
			break;
		if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))))
			step = "CreateCommandAllocator";
		else if (FAILED(hr = device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback))))
			step = "creating a readback buffer";
	}
	if (!*step && FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots[0].allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
		step = "CreateCommandList";
	if (!*step) {
		list->Close();
		D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srvHeap))))
			step = "CreateDescriptorHeap";
		heap = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
		if (!*step && FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap))))
			step = "CreateDescriptorHeap";
	}
	if (!*step && !MakePipeline(device.Get())) {
		step = "making the pipeline";
		hr = E_FAIL;
	}
	if (*step) {
		OOVR_LOGF("VR view window: %s failed (0x%08x), so it stays empty", step, (unsigned)hr);
		ReleaseResources();
		return false;
	}

	// Views: the band at t0; the frame's render target
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(band.Get(), &srv, srvHeap->GetCPUDescriptorHandleForHeapStart());
	device->CreateRenderTargetView(target.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());

	queue = gameQueue;
	eyeWidth = eye.Width;
	eyeHeight = eye.Height;
	eyeFormat = eye.Format;
	{
		std::lock_guard<std::mutex> guard(frameMutex);
		frame.assign((size_t)kRowPitch * kHeight, 0);
	}
	OOVR_LOGF("VR view window: showing %ux%u of the %llux%u eye image (format %d) at %ux%u", width, height, (unsigned long long)eye.Width, eye.Height, eye.Format, kWidth, kHeight);
	return true;
}

void MirrorWindow::Harvest()
{
	Slot* newest = nullptr;
	UINT64 done = fence->GetCompletedValue();
	for (Slot& s : slots) {
		if (s.pending && s.fenceValue <= done) {
			s.pending = false;
			if (!newest || s.fenceValue > newest->fenceValue)
				newest = &s;
		}
	}
	if (!newest)
		return;
	void* mapped = nullptr;
	D3D12_RANGE read{ 0, (SIZE_T)kRowPitch * kHeight };
	if (FAILED(newest->readback->Map(0, &read, &mapped)))
		return;
	{
		std::lock_guard<std::mutex> guard(frameMutex);
		memcpy(frame.data(), mapped, frame.size());
		haveFrame = true;
	}
	D3D12_RANGE written{ 0, 0 };
	newest->readback->Unmap(0, &written);
	if (HWND window = hwnd.load())
		InvalidateRect(window, nullptr, FALSE);
}

void MirrorWindow::Submit(ID3D12Resource* eye, ID3D12CommandQueue* eyeQueue)
{
	if (failed || closedByUser || !eye || !eyeQueue)
		return;

	if (!hwnd.load()) {
		ReleaseResources(); // the old window's, if the window went
		if (!OpenWindow()) {
			failed = true;
			return;
		}
	}
	HWND window = hwnd.load();
	if (!window || IsIconic(window))
		return; // closing, or minimised: nothing to show

	D3D12_RESOURCE_DESC desc = eye->GetDesc();
	if (!fence || desc.Width != eyeWidth || desc.Height != eyeHeight || desc.Format != eyeFormat || queue.Get() != eyeQueue) {
		if (!CreateResources(eyeQueue, desc)) {
			failed = true;
			return;
		}
	}

	Harvest();

	// At most kMaxFps frames a second, and only into a free readback buffer: otherwise the frame is skipped
	unsigned long long now = (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (now - lastFrameTick < 1000000ull / kMaxFps - 1000)
		return;
	Slot* slot = nullptr;
	for (Slot& s : slots)
		if (!s.pending) {
			slot = &s;
			break;
		}
	if (!slot)
		return;
	lastFrameTick = now;

	slot->allocator->Reset();
	list->Reset(slot->allocator.Get(), pipeline.Get());

	// The eye texture is read as the headset copy reads it (DX12Compositor::CopyToSwapchain): no barrier of its own
	D3D12_TEXTURE_COPY_LOCATION dst{ band.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src{ eye, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	src.SubresourceIndex = 0;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, &bandBox);

	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = band.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	list->ResourceBarrier(1, &barrier);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	D3D12_VIEWPORT viewport{ 0, 0, (float)kWidth, (float)kHeight, 0, 1 };
	D3D12_RECT scissor{ 0, 0, (LONG)kWidth, (LONG)kHeight };
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	list->SetGraphicsRootSignature(root.Get());
	ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
	list->SetDescriptorHeaps(1, heaps);
	list->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
	float constants[4] = { (float)kWidth, (float)kHeight, encodeSrgb ? 1.0f : 0.0f, 0.0f };
	list->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->DrawInstanced(3, 1, 0, 0);

	D3D12_RESOURCE_BARRIER after[2] = { barrier, barrier };
	after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // the band, ready for the next copy
	after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	after[1].Transition.pResource = target.Get();
	after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	list->ResourceBarrier(2, after);

	D3D12_TEXTURE_COPY_LOCATION rbDst{ slot->readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
	rbDst.PlacedFootprint.Offset = 0;
	rbDst.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, 1, kRowPitch };
	D3D12_TEXTURE_COPY_LOCATION rbSrc{ target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	rbSrc.SubresourceIndex = 0;
	list->CopyTextureRegion(&rbDst, 0, 0, 0, &rbSrc, nullptr);

	after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	after[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	list->ResourceBarrier(1, &after[1]);
	list->Close();

	ID3D12CommandList* lists[] = { list.Get() };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.Get(), nextFenceValue);
	slot->fenceValue = nextFenceValue++;
	slot->pending = true;
}

#endif
