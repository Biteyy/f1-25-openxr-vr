#pragma once

#include "XrDriverPrivate.h"

#include <openxr/openxr.h>

#include <string>

/**
 * Helmet view: the visor's tint, drawn head-locked over the game's whole image so the world is seen through the
 * visor. The visor is the Swofty addon's "visor" setting (see AddonSettings), which can change while the game runs.
 *
 * Direct3D 12 only (F1 25's API); with any other graphics API it logs once and stays off.
 */
class HelmetView {
public:
	~HelmetView();

	/**
	 * The quad layer to draw on top of the scene this frame, or nullptr when the visor is off or failed to load.
	 * Picks up visor changes; needs an active session on the application's graphics API.
	 */
	const XrCompositionLayerBaseHeader* GetLayer(const void* graphicsBinding);

	/** Destroys the swapchain. Call before the session it belongs to is destroyed. */
	void Reset();

private:
	// Loads helmet_visor_<visor>.png into the swapchain (made or remade to the image's size)
	bool Show(const void* graphicsBinding, const std::string& visor);

	XrSwapchain chain = XR_NULL_HANDLE;
	uint32_t chainWidth = 0, chainHeight = 0;
	XrCompositionLayerQuad layer{ XR_TYPE_COMPOSITION_LAYER_QUAD };

	// The setting generation last acted on (~0u: none yet), and whether that visor is showing
	unsigned int shownGeneration = ~0u;
	bool showing = false;

	// Set when the graphics API can't carry the layer, so nothing is retried every frame
	bool unsupported = false;
};
