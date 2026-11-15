#pragma once

#include "XrDriverPrivate.h"

#include <openxr/openxr.h>

#include <string>
#include <vector>

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

	/** The visor image being shown (sRGB RGBA, straight alpha), for the VR view window; null while there is none.
	 * generation changes whenever the image does. */
	const unsigned char* Image(unsigned& width, unsigned& height, unsigned& generation) const
	{
		width = imageWidth, height = imageHeight, generation = imageGeneration;
		return showing && !image.empty() ? image.data() : nullptr;
	}

	/** Half the angle the image spans across, in degrees (it is laid on a flat plane, see HelmetView.cpp). */
	static float HalfAngleDeg();

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

	// A copy of the shown image, for the VR view window
	std::vector<unsigned char> image;
	unsigned imageWidth = 0, imageHeight = 0, imageGeneration = 0;
};
