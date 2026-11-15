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

// The "visor" setting's values (swofty.json); tints in linear colour
const Named kNamed[] = {
	{ "clear", { { 0.0f, 0.0f, 0.0f }, 0.05f, false } },
	{ "light_smoke", { { 0.0f, 0.0f, 0.0f }, 0.30f, false } },
	{ "dark_smoke", { { 0.0f, 0.0f, 0.0f }, 0.55f, false } },
	{ "yellow", { { 0.80f, 0.55f, 0.00f }, 0.25f, false } },
	{ "green", { { 0.02f, 0.35f, 0.05f }, 0.30f, false } },
	{ "iridium_blue", { { 0.01f, 0.06f, 0.35f }, 0.35f, true } },
	{ "iridium_gold", { { 0.45f, 0.25f, 0.01f }, 0.35f, true } },
	{ "iridium_red", { { 0.40f, 0.03f, 0.01f }, 0.35f, true } },
};

float ToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
unsigned char ToSrgb8(float c)
{
	c = std::clamp(c, 0.0f, 1.0f);
	c = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
	return (unsigned char)std::lround(c * 255.0f);
}

float Smooth(float lo, float hi, float x)
{
	float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// Rotates a colour's hue around the grey axis by degrees (keeps its brightness roughly)
void Hue(const float in[3], float degrees, float out[3])
{
	float a = degrees * kPi / 180.0f, c = std::cos(a), s = std::sin(a), k = (1.0f - c) / 3.0f, r = std::sqrt(1.0f / 3.0f) * s;
	out[0] = in[0] * (c + k) + in[1] * (k - r) + in[2] * (k + r);
	out[1] = in[0] * (k + r) + in[1] * (c + k) + in[2] * (k - r);
	out[2] = in[0] * (k - r) + in[1] * (k + r) + in[2] * (c + k);
	for (int i = 0; i < 3; i++)
		out[i] = std::max(0.0f, out[i]);
}

// Smooth value noise, 0..1
float Hash(int x, int y)
{
	unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
float Noise(float x, float y)
{
	int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
	float fx = x - x0, fy = y - y0;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	float a = Hash(x0, y0) + (Hash(x0 + 1, y0) - Hash(x0, y0)) * fx;
	float b = Hash(x0, y0 + 1) + (Hash(x0 + 1, y0 + 1) - Hash(x0, y0 + 1)) * fx;
	return a + (b - a) * fy;
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
	// "rgba_RRGGBBAA": a plain tint, sRGB
	if (id.size() == 13 && id.compare(0, 5, "rgba_") == 0 && id.find_first_not_of("0123456789abcdef", 5) == std::string::npos) {
		unsigned long packed = std::strtoul(id.c_str() + 5, nullptr, 16);
		for (int i = 0; i < 3; i++)
			out.tint[i] = ToLinear((float)((packed >> (24 - 8 * i)) & 0xff) / 255.0f);
		out.opacity = (float)(packed & 0xff) / 255.0f;
		out.iridium = false;
		return true;
	}
	return false;
}

void Build(const Visor& visor, unsigned size, float halfAngleDeg, std::vector<unsigned char>& rgba)
{
	rgba.assign((size_t)size * size * 4, 0);
	const float span = std::tan(halfAngleDeg * kPi / 180.0f);

	// Iridium: the colour drifts to one neighbouring hue across the visor and to the other along its diagonal
	float warm[3], cool[3];
	Hue(visor.tint, 28.0f, warm);
	Hue(visor.tint, -28.0f, cool);

	// A darker tint shows its gradient and smudges more; a clear one still shows them a little
	const float base = visor.opacity;
	const float strength = 0.35f + 0.65f * std::min(1.0f, base / 0.35f);

	for (unsigned y = 0; y < size; y++) {
		for (unsigned x = 0; x < size; x++) {
			// Direction through the plane: right and up in degrees from straight ahead
			float u = ((x + 0.5f) / size * 2.0f - 1.0f) * span;
			float v = (1.0f - (y + 0.5f) / size * 2.0f) * span;
			float azimuth = std::atan(u) * 180.0f / kPi;
			float elevation = std::atan2(v, std::sqrt(1.0f + u * u)) * 180.0f / kPi;

			// Opacity: heavier towards the top (a gradient band from just above the eyes), a touch lighter below,
			// and heavier at the far sides
			float alpha = base;
			alpha += (0.16f + 0.12f * base) * Smooth(8.0f, 38.0f, elevation) * strength;
			alpha -= 0.04f * Smooth(-5.0f, -35.0f, elevation) * strength;
			alpha += 0.08f * Smooth(35.0f, 75.0f, std::fabs(azimuth)) * strength;
			// Faint smudges in the plastic
			float n = Noise(azimuth / 9.0f + 3.1f, elevation / 9.0f + 7.7f) * 0.6f + Noise(azimuth / 3.5f, elevation / 3.5f) * 0.4f;
			alpha += (n - 0.5f) * 0.03f * strength;

			// Colour
			float c[3] = { visor.tint[0], visor.tint[1], visor.tint[2] };
			if (visor.iridium) {
				float across = std::clamp(azimuth / 60.0f, -1.0f, 1.0f); // -1 left .. 1 right
				float diag = std::clamp((azimuth + elevation) / 90.0f, -1.0f, 1.0f);
				for (int i = 0; i < 3; i++) {
					float side = across > 0 ? warm[i] : cool[i];
					c[i] = c[i] + (side - c[i]) * std::fabs(across) * 0.6f;
					c[i] = c[i] + ((diag > 0 ? cool[i] : warm[i]) - c[i]) * std::fabs(diag) * 0.25f;
				}
			}
			// Darker at the top: the gradient band is a deeper shade, not just more of the same
			float deepen = 1.0f - 0.35f * Smooth(8.0f, 38.0f, elevation);
			for (int i = 0; i < 3; i++)
				c[i] *= deepen;

			alpha = std::clamp(alpha, 0.0f, 0.92f);

			// The helmet round the visor's opening, out of focus: its brow above, the chin bar below, closing in a
			// little towards the far sides. A rounded rectangle in angles, opaque outside with a soft edge.
			// Worked on the image plane itself (tangents), so the edges come out as smooth lines: the brow straight
			// across at about 33 degrees up, dipping a little at the sides; the chin bar at about 42 down (below the wheel), rising
			// towards the sides; the opening ends well past the sides of a headset's view.
			float ax = std::fabs(u);
			float top = 0.65f * (1.0f - 0.10f * std::min(ax / 1.4f, 1.0f) * std::min(ax / 1.4f, 1.0f));
			float bottom = 0.90f * (1.0f - 0.18f * std::min(ax / 1.4f, 1.0f) * std::min(ax / 1.4f, 1.0f));
			float ey = v >= 0 ? v / top : -v / bottom;
			float ex = ax / 2.6f;
			float shape = std::pow(std::pow(ex, 4.0f) + std::pow(ey, 4.0f), 0.25f);
			float helmet = Smooth(0.90f, 1.10f, shape);
			// The lining: near black, a shade lighter just at the edge where light from the opening falls on it
			float lit = (1.0f - Smooth(1.0f, 1.25f, shape)) * helmet;
			float shell[3] = { 0.012f + 0.035f * lit, 0.012f + 0.035f * lit, 0.014f + 0.04f * lit };

			unsigned char* p = &rgba[((size_t)y * size + x) * 4];
			for (int i = 0; i < 3; i++)
				p[i] = ToSrgb8(c[i] + (shell[i] - c[i]) * helmet);
			p[3] = (unsigned char)std::lround((alpha + (1.0f - alpha) * helmet) * 255.0f);
		}
	}
}

} // namespace VisorLook
