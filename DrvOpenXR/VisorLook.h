#pragma once

/**
 * The helmet visor as a filter (see VisorPass): how much of each colour of light it lets through, and where the
 * helmet round its opening blocks the view (the lining shows, dark and out of focus, as the brow above and the chin
 * bar below, under the steering wheel).
 *
 * A tinted visor passes its own colour. An iridium (mirrored) visor reflects its coating's colour outwards, so from
 * inside it reads as a smoke with the opposite cast: a blue iridium a little warm, a gold one a little blue. Every
 * visor is darker in a band at the top (the sun strip) and a touch at the far sides.
 *
 * No graphics here: it fills an RGBA8 image for VisorPass to upload.
 */

#include <string>
#include <vector>

namespace VisorLook {

struct Visor {
	float transmission[3]; // linear 0..1, straight ahead
};

/** The visor of an id from the "visor" setting ("dark_smoke", ...), or of "rgba_RRGGBBAA" (the outside colour and
 * darkness of the player's driver's visor, sRGB, from the plugin). False for any other id. */
bool Find(const std::string& id, Visor& out);

/**
 * Draws the visor map into rgba (size x size pixels): RGB the light let through (linear), A the helmet (1 where it
 * blocks the view). The image is laid on a flat plane spanning +-halfAngleDeg from straight ahead, across and
 * up/down, so each pixel's direction is taken through that plane.
 */
void Build(const Visor& visor, unsigned size, float halfAngleDeg, std::vector<unsigned char>& rgba);

} // namespace VisorLook
