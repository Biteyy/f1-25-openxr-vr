#pragma once

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

/**
 * VR view window: a desktop window showing the left eye as the headset shows it (VisorPass's copy, with the visor,
 * rain and spray), cropped to 16:9, for screen sharing or recording.
 *
 * The window is an ordinary GDI window: each frame is drawn at 1280x720 on the game's queue, read back, and painted
 * with StretchDIBits. That way every capture method sees it (Discord, OBS window capture, Teams, PrintWindow and
 * BitBlt), where a flip-model swapchain shows black to the older ones. It lives in the game's process, so a share
 * "with sound" carries the game's audio.
 *
 * The window runs its own message thread and never takes focus from the game. A frame is skipped, never waited for,
 * when every readback buffer is still in flight; at most kMaxFps frames a second are drawn.
 */
class MirrorWindow {
public:
	MirrorWindow() = default;
	MirrorWindow(const MirrorWindow&) = delete;
	MirrorWindow& operator=(const MirrorWindow&) = delete;
	~MirrorWindow();

	/** Shows the eye image (opening the window first if needed): read on queue without a barrier, so it must be in a
	 * state a copy can read. Call from the thread that submits the eye. */
	void Submit(ID3D12Resource* eye, ID3D12CommandQueue* queue);

	/** Closes the window. The next Submit opens it again. */
	void Close();

	/** True once the player closed the window themselves; Close() clears it. */
	bool ClosedByUser() const { return closedByUser; }

	static const UINT kWidth = 1280, kHeight = 720;

private:
	bool OpenWindow();
	bool CreateResources(ID3D12CommandQueue* queue, const D3D12_RESOURCE_DESC& eye);
	bool MakePipeline(ID3D12Device* device);
	void ReleaseResources();
	void Harvest(); // copies finished readbacks into the window's frame
	void Paint(HWND window);
	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	static const UINT kSlots = 3;
	static const UINT kMaxFps = 60;
	static const UINT kRowPitch = kWidth * 4; // a multiple of D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

	std::thread thread;
	std::atomic<HWND> hwnd{ nullptr };
	std::atomic<bool> closedByUser{ false };

	// The last finished frame, BGRA top-down, painted by the window thread
	std::mutex frameMutex;
	std::vector<unsigned char> frame;
	bool haveFrame = false;

	Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
	Microsoft::WRL::ComPtr<ID3D12Fence> fence;
	UINT64 nextFenceValue = 1;
	struct Slot {
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
		Microsoft::WRL::ComPtr<ID3D12Resource> readback;
		UINT64 fenceValue = 0;
		bool pending = false;
	} slots[kSlots];
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
	Microsoft::WRL::ComPtr<ID3D12Resource> band; // the eye's 16:9 band, copied as it is (the eye texture has no view of ours)
	Microsoft::WRL::ComPtr<ID3D12Resource> target; // kWidth x kHeight BGRA8: the band scaled
	Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap; // srv: the band
	unsigned long long lastFrameTick = 0;

	// The eye texture the resources were made for, and the band of it that is shown
	UINT64 eyeWidth = 0;
	UINT eyeHeight = 0;
	DXGI_FORMAT eyeFormat = DXGI_FORMAT_UNKNOWN;
	D3D12_BOX bandBox = {};
	bool encodeSrgb = false; // the eye is linear floating point: encode it for the screen

	bool failed = false; // set after an error, so it isn't retried every frame; Close() clears it
};

#endif
