#pragma once

/**
 * Draws RainSim's sprites with Direct3D 12 on the game's own device and queue: textured quads (a soft mist puff, a
 * water drop as a normal map, or flat) blended premultiplied: the spray straight into the eye image, the drops into
 * VisorPass's water map. No OpenXR in here, so it can be tried on its own.
 */

#include "RainSim.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <vector>

class RainDraw {
public:
	/** One viewport's worth: a rectangle of the target, its camera, and the sprites to draw there */
	struct Pass {
		D3D12_RECT rect;
		float viewProj[16]; // row-major, world (or visor plane) to clip space
		float right[3], up[3]; // the camera's axes the sprites face along
		const std::vector<Rain::Sprite>* sprites;
	};

	~RainDraw();

	/**
	 * Draws the passes into the target (cleared to transparent first if clear) and submits that on the queue (without
	 * waiting). target must be in the render target state, and stays in it; format is the typed format to draw it
	 * as. Sets things up on first use; false if that failed (logged once) or the device changed.
	 */
	bool Render(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* target, DXGI_FORMAT format,
	    const Pass* passes, size_t passCount, bool clear);

	/** Waits for the GPU to finish with everything submitted, then frees it all. */
	void Release();

	/** The camera matrices for an eye: OpenXR pose (orientation x,y,z,w and position) and field of view angles. */
	static void EyeCamera(const float orientation[4], const float position[3], float angleLeft, float angleRight,
	    float angleUp, float angleDown, Pass& pass);

	/** The camera for the visor plane: x, y in metres, halfSize to the image's edges. */
	static void PlaneCamera(float halfSize, Pass& pass);

private:
	bool Setup(ID3D12Device* device, ID3D12CommandQueue* queue);
	ID3D12PipelineState* PipelineFor(DXGI_FORMAT format);
	bool UploadAtlas(ID3D12CommandQueue* queue);
	void WaitIdle();

	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	ID3D12Device* device = nullptr; // the game's; not held
	bool failed = false;

	ComPtr<ID3D12RootSignature> rootSignature;
	ComPtr<ID3DBlob> vertexShader, pixelShader;
	// One pipeline per target format (the eye image's, the water map's)
	static const int kFormats = 4;
	ComPtr<ID3D12PipelineState> pipelines[kFormats];
	DXGI_FORMAT pipelineFormats[kFormats] = {};

	ComPtr<ID3D12Resource> atlas;
	ComPtr<ID3D12DescriptorHeap> srvHeap;
	ComPtr<ID3D12DescriptorHeap> rtvHeap; // one RTV, rewritten each frame for that frame's target

	// A few frames in flight (three draws a frame), each with its own commands and upload memory
	static const int kSlots = 9;
	struct Slot {
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Resource> upload;
		unsigned char* mapped = nullptr;
		UINT64 fenceValue = 0;
	};
	Slot slots[kSlots];
	int nextSlot = 0;
	ComPtr<ID3D12Fence> fence;
	UINT64 fenceCounter = 0;
	HANDLE fenceEvent = nullptr;
	ID3D12CommandQueue* lastQueue = nullptr;
};

#endif
