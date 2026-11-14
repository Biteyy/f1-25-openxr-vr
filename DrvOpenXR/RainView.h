#pragma once

#include "XrDriverPrivate.h"

#include "RainSim.h"

#include <openxr/openxr.h>

#include <chrono>
#include <vector>

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
#include "RainDraw.h"
#endif

/**
 * Tyre spray and visor rain drops (see RainSim) as two extra layers over the game's image: the spray as a
 * projection layer drawn with the game's own eye poses, and the drops (and the veil inside a cloud) as a
 * head-locked quad under the visor tint. Both live in one swapchain, drawn by RainDraw on the game's D3D12 queue.
 *
 * Direct3D 12 only; with any other graphics API it logs once and stays off.
 */
class RainView {
public:
	~RainView();

	/**
	 * Steps the simulation and, while there is spray or drops, draws them and appends their layers to layers.
	 * gameViews: the two eye views the game's frame is submitted with; space: that layer's space.
	 */
	void AddLayers(const void* graphicsBinding, const XrCompositionLayerProjectionView* gameViews, XrSpace space,
	    std::vector<const XrCompositionLayerBaseHeader*>& layers);

	/** Destroys the swapchain and the drawing state. Call before the session it belongs to is destroyed. */
	void Reset();

private:
	Rain::Sim sim;
	std::vector<Rain::Sprite> spray, drops;
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), last{};
	bool loggedActive = false;
	bool unsupported = false;

	XrSwapchain chain = XR_NULL_HANDLE;
	int64_t chainFormat = 0;
	uint32_t chainWidth = 0, chainHeight = 0, eyeWidth = 0, eyeHeight = 0, dropSize = 0;
	std::vector<void*> images; // ID3D12Resource*, owned by the swapchain

	XrCompositionLayerProjectionView views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	XrCompositionLayerProjection sprayLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerQuad dropLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	RainDraw draw;
	bool MakeSwapchain(uint32_t eyeW, uint32_t eyeH);
#endif
};
