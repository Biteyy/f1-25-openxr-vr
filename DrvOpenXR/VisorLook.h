#pragma once

/**
 * The helmet visor's image (see HelmetView): a tint that isn't one flat colour, as a real visor isn't. It is
 * darker towards the top (visors are tinted heavier above the eyes) and a little at the sides (the curved plastic is
 * looked through at a slant there), an iridium visor shifts colour across its width, and there is a faint light
 * streak off the visor's curve, very faint smudges and the edge of the tear-offs near the top.
 *
 * No graphics here: it fills an RGBA8 image (sRGB colours, straight alpha) for HelmetView to upload.
 */

#include <string>
#include <vector>

namespace VisorLook {

struct Visor {
	float tint[3]; // linear 0..1
	float opacity; // straight-ahead opacity, 0..1
	bool iridium;
};

/** The look of a visor id from the "visor" setting ("dark_smoke", ...), or of "rgba_RRGGBBAA" (a plain tint in
 * sRGB, from the plugin for the player's driver's helmet). False for any other id. */
bool Find(const std::string& id, Visor& out);

/**
 * Draws the visor into rgba (size x size pixels). The image is laid on a flat plane spanning +-halfAngleDeg from
 * straight ahead, across and up/down, so each pixel's direction is taken through that plane.
 */
void Build(const Visor& visor, unsigned size, float halfAngleDeg, std::vector<unsigned char>& rgba);

} // namespace VisorLook
