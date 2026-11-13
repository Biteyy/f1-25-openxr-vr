#pragma once

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>

namespace vr {
struct D3D12TextureData_t;
}

/**
 * VR view window: a desktop window showing the game's left-eye image as it renders it for the headset, cropped to
 * 16:9, for screen sharing or recording. It shows the game's image only, so the helmet visor (drawn later, by the
 * headset's compositor) isn't in it.
 *
 * The window runs its own message thread and never takes focus from the game. Each frame costs one copy on the
 * game's queue and a present that never waits: a frame the window isn't ready for is skipped.
 */
class MirrorWindow {
public:
	MirrorWindow() = default;
	MirrorWindow(const MirrorWindow&) = delete;
	MirrorWindow& operator=(const MirrorWindow&) = delete;
	~MirrorWindow();

	/** Shows the eye image (opening the window first if needed). Call from the thread that submits the eye. */
	void Submit(const vr::D3D12TextureData_t* eye);

	/** Closes the window. The next Submit opens it again. */
	void Close();

	/** True once the player closed the window themselves; Close() clears it. */
	bool ClosedByUser() const { return closedByUser; }

private:
	bool OpenWindow();
	bool CreateChain(ID3D12CommandQueue* queue, const D3D12_RESOURCE_DESC& eye);
	void ReleaseChain();
	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	static const UINT kBuffers = 2;

	std::thread thread;
	DWORD threadId = 0;
	std::atomic<HWND> hwnd{ nullptr };
	std::atomic<bool> closedByUser{ false };

	Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
	Microsoft::WRL::ComPtr<IDXGISwapChain3> chain;
	HANDLE frameWait = nullptr;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators[kBuffers];
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
	Microsoft::WRL::ComPtr<ID3D12Fence> fence;
	UINT64 fenceValues[kBuffers] = {};
	UINT64 nextFenceValue = 1;

	// The eye texture the chain was made for, and the 16:9 band of it that is shown
	UINT64 eyeWidth = 0;
	UINT eyeHeight = 0;
	DXGI_FORMAT eyeFormat = DXGI_FORMAT_UNKNOWN;
	D3D12_BOX band = {};

	bool failed = false; // set after an error, so it isn't retried every frame; Close() clears it
};

#endif
