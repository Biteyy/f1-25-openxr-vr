#include "VisorLook.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace VisorLook {

namespace {

const float kPi = 3.14159265f;

struct Named {
	const char* id;
	Visor visor;
};

// The "visor" setting's values (swofty.json): the light each lets through, linear. Iridium visors reflect their
// coating's colour, so they pass a smoke with the opposite cast.
const Named kNamed[] = {
	{ "clear", { { 0.92f, 0.92f, 0.92f } } },
	{ "light_smoke", { { 0.62f, 0.62f, 0.62f } } },
	{ "dark_smoke", { { 0.38f, 0.38f, 0.39f } } },
	{ "yellow", { { 0.95f, 0.84f, 0.32f } } }, // cuts the blue: the world goes warm and contrasty
	{ "green", { { 0.40f, 0.66f, 0.42f } } },
	{ "iridium_blue", { { 0.56f, 0.50f, 0.38f } } },
	{ "iridium_gold", { { 0.40f, 0.47f, 0.56f } } },
	{ "iridium_red", { { 0.36f, 0.50f, 0.53f } } },
	{ "amber", { { 0.90f, 0.62f, 0.22f } } }, // a deeper yellow: strong contrast, warm
	{ "rose", { { 0.82f, 0.55f, 0.58f } } },
	{ "blue", { { 0.45f, 0.58f, 0.85f } } },
	{ "iridium_silver", { { 0.30f, 0.30f, 0.31f } } }, // a mirror: dark and neutral
	{ "iridium_green", { { 0.52f, 0.38f, 0.48f } } },
	{ "iridium_purple", { { 0.44f, 0.52f, 0.36f } } },
};

float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float Smooth(float lo, float hi, float x)
{
	float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

} // namespace

bool Find(const std::string& id, Visor& out)
{
	for (const Named& n : kNamed) {
		if (id == n.id) {
			out = n.visor;
			return true;
		}
	}
	// "rgba_RRGGBBAA": the driver's visor as it looks from outside (its texture's colour, sRGB) and how dark it is.
	// From inside it is mostly a smoke of that darkness: a tinted visor's own colour shows through a little, and an
	// iridium one reads as smoke, so only a quarter of the colour is kept, and at most a quarter either way.
	if (id.size() == 13 && id.compare(0, 5, "rgba_") == 0 && id.find_first_not_of("0123456789abcdef", 5) == std::string::npos) {
		unsigned long packed = std::strtoul(id.c_str() + 5, nullptr, 16);
		float c[3];
		for (int i = 0; i < 3; i++)
			c[i] = ToLinear((float)((packed >> (24 - 8 * i)) & 0xff) / 255.0f);
		float darkness = (float)(packed & 0xff) / 255.0f;
		float mean = (c[0] + c[1] + c[2]) / 3.0f;
		float smoke = 1.0f - 0.85f * darkness;
		for (int i = 0; i < 3; i++) {
			float hue = mean > 0.005f ? std::clamp(1.0f + (c[i] / mean - 1.0f) * 0.25f, 0.75f, 1.25f) : 1.0f;
			out.transmission[i] = std::clamp(smoke * hue, 0.05f, 1.0f);
		}
		return true;
	}
	return false;
}

void Build(const Visor& visor, unsigned size, float halfAngleDeg, std::vector<unsigned char>& rgba)
{
	rgba.assign((size_t)size * size * 4, 0);
	const float span = std::tan(halfAngleDeg * kPi / 180.0f);
	const float average = (visor.transmission[0] + visor.transmission[1] + visor.transmission[2]) / 3.0f;

	for (unsigned y = 0; y < size; y++) {
		for (unsigned x = 0; x < size; x++) {
			// Direction through the plane: right and up in degrees from straight ahead
			float u = ((x + 0.5f) / size * 2.0f - 1.0f) * span;
			float v = (1.0f - (y + 0.5f) / size * 2.0f) * span;
			float azimuth = std::atan(u) * 180.0f / kPi;
			float elevation = std::atan2(v, std::sqrt(1.0f + u * u)) * 180.0f / kPi;

			// The sun strip at the top (deeper on a dark visor, a hint on a clear one) and the far sides
			float band = Smooth(8.0f, 36.0f, elevation) * (0.22f + 0.28f * (1.0f - average));
			float sides = 0.08f * Smooth(35.0f, 75.0f, std::fabs(azimuth));
			float keep = (1.0f - band) * (1.0f - sides);

			// The helmet round the visor's opening, out of focus: its brow above, the chin bar below, closing in a
			// little towards the far sides. A rounded rectangle in tangents, so the edges come out as smooth lines:
			// the brow straight across at about 33 degrees up, the chin bar at about 42 down (below the wheel).
			float ax = std::fabs(u);
			float top = 0.65f * (1.0f - 0.10f * std::min(ax / 1.4f, 1.0f) * std::min(ax / 1.4f, 1.0f));
			float bottom = 0.90f * (1.0f - 0.18f * std::min(ax / 1.4f, 1.0f) * std::min(ax / 1.4f, 1.0f));
			float ey = v >= 0 ? v / top : -v / bottom;
			float ex = ax / 2.6f;
			float shape = std::pow(std::pow(ex, 4.0f) + std::pow(ey, 4.0f), 0.25f);
			float helmet = Smooth(0.90f, 1.10f, shape);

			unsigned char* p = &rgba[((size_t)y * size + x) * 4];
			for (int i = 0; i < 3; i++)
				p[i] = (unsigned char)std::lround(std::clamp(visor.transmission[i] * keep, 0.0f, 1.0f) * 255.0f);
			p[3] = (unsigned char)std::lround(helmet * 255.0f);
		}
	}
}

} // namespace VisorLook
