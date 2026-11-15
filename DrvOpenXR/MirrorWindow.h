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
 * 16:9, for screen sharing or recording, with the helmet visor drawn over it as the headset shows it (the headset's
 * compositor adds the visor later, so the window draws it itself). The tyre spray isn't in it.
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

	/** The visor to draw over the image: HelmetView's image and the left eye's field of view (tangents). */
	struct Visor {
		const unsigned char* rgba = nullptr; // sRGB, straight alpha; null: no visor
		unsigned width = 0, height = 0, generation = 0;
		float halfAngleDeg = 80.0f; // the image spans +- this on a flat plane
		float tanLeft = -1, tanRight = 1, tanUp = 1, tanDown = -1;
	};

	/** Shows the eye image (opening the window first if needed), with the visor over it. Call from the thread that
	 * submits the eye. */
	void Submit(const vr::D3D12TextureData_t* eye, const Visor& visor);

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

	// Drawing the visor over the copy: a full-window triangle sampling the visor image
	bool MakeVisorPipeline(DXGI_FORMAT format);
	bool UploadVisor(const Visor& visor);
	Microsoft::WRL::ComPtr<ID3D12RootSignature> visorRoot;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> visorPipeline;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> visorSrvHeap, rtvHeap;
	Microsoft::WRL::ComPtr<ID3D12Resource> visorTexture;
	unsigned visorGeneration = ~0u;
	bool visorBroken = false; // the visor couldn't be set up: the window shows the plain image
};

#endif
