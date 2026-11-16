#pragma once

/**
 * The view out through the helmet, drawn into each eye image as the headset gets it: Direct3D 12, on the game's own
 * queue, in place of the plain copy of the game's image into the OpenXR swapchain (DX12Compositor's eyePass).
 *
 * For each pixel, in the order light goes through them:
 *   - the tyre spray (RainSim, drawn by RainDraw at half resolution) and, inside a cloud, the veil;
 *   - the water drops on the visor: lenses that show a small, blurred, upside-down bit of the world, darker at
 *     their rim and with the sky's highlight (RainDraw draws them into a normal map);
 *   - the outer tear-off: rubber and dust specks, spray grime; while one is pulled it slides off to the left with
 *     its drops, and the clean one under it shows;
 *   - misting at the bottom of the visor (wet, slow);
 *   - the visor's tint as a filter (VisorLook), and the helmet round the opening.
 * The drops and the tear-off are worked out on RainSim's visor plane from each pixel's direction, so both eyes see
 * them in the same place: they sit on the view like marks on a lens (no stereo depth to make a bubble of them).
 *
 * No OpenXR in here, so it can be tried on its own.
 */

#include "RainDraw.h"
#include "RainSim.h"

#if defined(_WIN32)

#include <chrono>
#include <string>
#include <vector>

class VisorPass {
public:
	/** One eye's camera as the game drew it: OpenXR orientation (x, y, z, w) and position, field of view angles */
	struct EyeView {
		float orientation[4], position[3];
		float angleLeft, angleRight, angleUp, angleDown;
	};

	/** What the left eye (the first each frame) brings */
	struct FrameInput {
		const Rain::Feed* feed = nullptr; // the plugin's cars and settings; null without a session
		std::string visor; // the visor id ("off", "dark_smoke", "rgba_RRGGBBAA", ...)
		unsigned visorGeneration = 0; // changes whenever visor does
		bool mirror = false; // keep a copy of the left eye for the VR view window
		float dt = -1; // seconds since the last frame; below 0: measured
	};

	~VisorPass();

	/**
	 * Draws the game's eye image src into the swapchain image dst (in the render target state, and left in it; format
	 * is its typed format) with everything on the visor over it, and submits that on queue. views: both eyes (the
	 * left eye's call draws the spray for both). input: read on the left eye only. False when it can't (logged once):
	 * the caller then copies the image as it is.
	 */
	bool Draw(int eye, ID3D12CommandQueue* queue, ID3D12Resource* src, ID3D12Resource* dst, DXGI_FORMAT format,
	    const EyeView views[2], const FrameInput& input);

	/** The left eye as last drawn, for the VR view window (in the copy source state), or null */
	ID3D12Resource* MirrorImage() const { return mirrorReady ? mirror.Get() : nullptr; }

	/** Waits for the GPU, then frees everything. Call before the game's device or queue may go. */
	void Release();

	/** The simulation, for the log */
	const Rain::Sim& Sim() const { return sim; }

private:
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	bool Setup(ID3D12Device* device);
	bool MakeTargets(const D3D12_RESOURCE_DESC& src, DXGI_FORMAT format);
	bool UploadVisor(ID3D12CommandQueue* queue, const std::string& visor);
	ID3D12PipelineState* PipelineFor(DXGI_FORMAT format);
	void WaitIdle();
	void Step(const FrameInput& input);

	ID3D12Device* device = nullptr; // the game's; not held
	ID3D12CommandQueue* lastQueue = nullptr;
	bool failed = false;

	ComPtr<ID3D12RootSignature> root;
	ComPtr<ID3DBlob> vs, ps;
	static const int kFormats = 2;
	ComPtr<ID3D12PipelineState> pipelines[kFormats];
	DXGI_FORMAT pipelineFormats[kFormats] = {};

	ComPtr<ID3D12DescriptorHeap> srvHeap; // per eye: its scene, the layers, the visor map
	ComPtr<ID3D12DescriptorHeap> rtvHeap; // the swapchain image's view, written each draw
	UINT srvStep = 0;

	// The game's eye images, copied (their state is the game's); the layers (both eyes' spray at half resolution,
	// then the drops' normal map); the visor map; the left eye as drawn, for the VR view window
	ComPtr<ID3D12Resource> scene[2], layers, visorMap, mirror;
	UINT64 eyeWidth = 0;
	UINT eyeHeight = 0;
	DXGI_FORMAT eyeFormat = DXGI_FORMAT_UNKNOWN;
	UINT sprayWidth = 0, sprayHeight = 0, layersWidth = 0, layersHeight = 0;
	bool layersAsTarget = true; // its state: render target (else shader resource)
	bool layersDrawn = false; // this frame has spray or drops in it
	bool mirrorReady = false;

	unsigned visorGeneration = ~0u;
	bool visorOn = false;
	bool visorPhotochromic = false;

	static const int kSlots = 4;
	struct Slot {
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list, pre;
		UINT64 fenceValue = 0;
	};
	Slot slots[kSlots];
	int nextSlot = 0;
	ComPtr<ID3D12Fence> fence;
	UINT64 fenceCounter = 0;
	HANDLE fenceEvent = nullptr;

	// The rain and the visor's state, stepped on the left eye
	Rain::Sim sim;
	Rain::VisorState state{};
	std::vector<Rain::Sprite> spray, drops;
	RainDraw draw;
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), last{};
	float time = 0;
	bool loggedRain = false;
	uint32_t loggedTearOffs = 0;
};

#endif
