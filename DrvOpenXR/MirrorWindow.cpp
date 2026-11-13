#include "MirrorWindow.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#include "XrDriverPrivate.h"
#include "generated/interfaces/vrtypes.h"

#include <algorithm>

#pragma comment(lib, "dxgi.lib")

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

void MirrorWindow::Submit(const vr::D3D12TextureData_t* eye)
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

	std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
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
