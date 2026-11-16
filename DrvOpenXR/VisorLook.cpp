#include "VisorLook.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <tuple>

namespace VisorLook {

namespace {

const float kPi = 3.14159265f;

// Neutral visors: the light each lets through, linear
struct Named {
	const char* id;
	float transmission;
};
const Named kNeutral[] = { { "clear", 0.92f }, { "light_smoke", 0.62f }, { "dark_smoke", 0.38f } };

// The "visor_colour" setting's values: the light a visor tinted that colour lets through, linear
struct Colour {
	const char* id;
	float tint[3];
};
const Colour kColours[] = {
	{ "yellow", { 0.95f, 0.84f, 0.32f } }, // cuts the blue: warm and contrasty
	{ "amber", { 0.90f, 0.62f, 0.22f } },
	{ "orange", { 0.92f, 0.50f, 0.18f } },
	{ "red", { 0.85f, 0.30f, 0.25f } },
	{ "rose", { 0.82f, 0.55f, 0.58f } },
	{ "magenta", { 0.78f, 0.40f, 0.72f } },
	{ "purple", { 0.55f, 0.38f, 0.78f } },
	{ "blue", { 0.45f, 0.58f, 0.85f } },
	{ "ice_blue", { 0.62f, 0.78f, 0.92f } },
	{ "teal", { 0.30f, 0.68f, 0.66f } },
	{ "green", { 0.40f, 0.66f, 0.42f } },
	{ "grey_green", { 0.42f, 0.50f, 0.40f } }, // like G-15 sunglasses
	{ "bronze", { 0.62f, 0.45f, 0.30f } },
	{ "gold", { 0.88f, 0.72f, 0.35f } },
	{ "silver", { 0.62f, 0.62f, 0.62f } },
};

// How dark an iridium visor is straight ahead (a mirror coating on a smoke)
const float kIridium = 0.52f, kIridiumDark = 0.32f;

float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float Smooth(float lo, float hi, float x)
{
	float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// The light a visor lets through for a colour: a tint passes it; an iridium mirror reflects it, so it passes a smoke
// of its darkness with the opposite cast
void Shade(Mode mode, float smoke, const float tint[3], float out[3])
{
	float mean = (tint[0] + tint[1] + tint[2]) / 3.0f;
	for (int i = 0; i < 3; i++) {
		if (mode == Mode::Tinted)
			out[i] = tint[i];
		else if (mode == Mode::Iridium)
			out[i] = smoke * std::clamp(1.0f - 0.5f * (tint[i] / mean - 1.0f), 0.6f, 1.4f);
		else
			out[i] = smoke;
	}
}

// A tint of hue degrees, for the rainbow visor (an oil-slick coating whose colour shifts across it)
void HueTint(float degrees, float out[3])
{
	for (int i = 0; i < 3; i++) {
		float k = std::fmod(degrees / 60.0f + (i == 0 ? 5.0f : i == 1 ? 3.0f : 1.0f), 6.0f);
		float f = std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f); // HSV with full saturation
		out[i] = 0.85f * (1.0f - 0.55f * f); // value 0.85, saturation 0.55
	}
}

} // namespace

bool Find(const std::string& id, Visor& out)
{
	out = Visor{};
	for (const Named& n : kNeutral) {
		if (id == n.id) {
			out.mode = Mode::Neutral;
			out.smoke = n.transmission;
			for (float& t : out.transmission)
				t = n.transmission;
			return true;
		}
	}
	if (id == "photochromic") {
		// Clear in the dark, about a dark smoke in bright sun: the pass darkens it from the light it sees
		out.mode = Mode::Neutral;
		out.photochromic = true;
		out.smoke = 1.0f;
		for (float& t : out.transmission)
			t = 1.0f;
		return true;
	}
	// "<type>_<colour>" (tinted, iridium, iridium_dark), from the plugin; a bare colour is tinted (the visor ids of
	// 1.4.1 and before, such as "yellow" and "iridium_gold", read the same way)
	std::string colour = id;
	out.mode = Mode::Tinted;
	for (auto [prefix, mode, smoke] : { std::tuple{ "tinted_", Mode::Tinted, 1.0f }, std::tuple{ "iridium_dark_", Mode::Iridium, kIridiumDark },
	         std::tuple{ "iridium_", Mode::Iridium, kIridium } }) {
		size_t n = std::strlen(prefix);
		if (id.compare(0, n, prefix) == 0) {
			colour = id.substr(n);
			out.mode = mode;
			out.smoke = smoke;
			break;
		}
	}
	if (colour == "rainbow") {
		out.rainbow = true;
		if (out.mode == Mode::Tinted)
			out.smoke = 0.75f;
		float tint[3];
		HueTint(200.0f, tint);
		Shade(out.mode, out.smoke, tint, out.transmission);
		return true;
	}
	for (const Colour& c : kColours) {
		if (colour == c.id) {
			Shade(out.mode, out.smoke, c.tint, out.transmission);
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

			// The rainbow coating's colour runs across the visor and up it diagonally
			float t[3] = { visor.transmission[0], visor.transmission[1], visor.transmission[2] };
			if (visor.rainbow) {
				float tint[3];
				HueTint(200.0f + azimuth * 2.2f + elevation * 1.4f, tint);
				Shade(visor.mode, visor.smoke, tint, t);
			}

			unsigned char* p = &rgba[((size_t)y * size + x) * 4];
			for (int i = 0; i < 3; i++)
				p[i] = (unsigned char)std::lround(std::clamp(t[i] * keep, 0.0f, 1.0f) * 255.0f);
			p[3] = (unsigned char)std::lround(helmet * 255.0f);
		}
	}
}

} // namespace VisorLook
