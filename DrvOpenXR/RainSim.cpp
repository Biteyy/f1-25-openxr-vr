#include "RainSim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Rain {

namespace {

const float kPi = 3.14159265f;

// The driver's eyes relative to the car's position (its origin, near the ground): up and ahead, metres
const float kEyeUp = 0.7f;
const float kEyeAhead = 0.0f;

// Most mist puffs and visor drops kept at once
const size_t kMaxMist = 6000;
const size_t kMaxBeads = 220;

// Cars further than this from the player throw no spray (it couldn't be seen)
const float kSprayRange = 250.0f;

Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
Vec3 Normalise(Vec3 a)
{
	float l = Length(a);
	return l > 1e-6f ? a * (1.0f / l) : Vec3{};
}
Vec3 FromArray(const float* v) { return { v[0], v[1], v[2] }; }
Vec3 Axis(int i) { return { i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f }; }
float Smoothstep(float lo, float hi, float x)
{
	float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

// A car's own frame from its velocity: forward, up (the world's, square to forward) and right
struct Frame {
	Vec3 forward, up, right;
};

bool CarFrame(const Car& car, Vec3 worldUp, float handedness, Frame& out)
{
	if (car.speed < 3.0f)
		return false;
	out.forward = Normalise(FromArray(car.vel));
	out.up = Normalise(worldUp - out.forward * Dot(worldUp, out.forward));
	if (Length(out.forward) < 0.5f || Length(out.up) < 0.5f)
		return false;
	out.right = Cross(out.up, out.forward) * handedness;
	return true;
}

} // namespace

Sim::Sim()
    : rng(20261114u)
{
	mist.reserve(kMaxMist);
	beads.reserve(kMaxBeads);
}

void Sim::Update(const Feed* feed, double seconds, float dt)
{
	dt = std::clamp(dt, 0.0f, 0.05f);

	// Off, light, normal, heavy: at full density the cloud blocked the view in a test drive, so normal is now a third of it
	static const float kSprayScales[4] = { 0.0f, 0.15f, 0.35f, 0.65f };
	if (feed) {
		sprayScale = kSprayScales[std::min(feed->spray, 3u)];
		dropsOn = feed->drops != 0;
		lastSteer = feed->steer;
		if (feed->seq != lastSeq || carCount == 0) {
			lastSeq = feed->seq;
			seqSeconds = seconds;
		}
	}

	// The snapshot is from the game's last frame; when it stops changing for a while the game is paused (or
	// loading), and everything holds still
	double age = seconds - seqSeconds;
	if (!feed || age > 0.25)
		dt = feed ? 0.0f : dt;

	carCount = 0;
	playerIndex = -1;
	if (feed && dt > 0) {
		float ahead = (float)std::clamp(age, 0.0, 0.1);
		for (uint32_t i = 0; i < feed->count && i < kMaxCars; i++) {
			Car c = feed->cars[i];
			for (int k = 0; k < 3; k++)
				c.pos[k] += c.vel[k] * ahead;
			if (c.player && playerIndex < 0)
				playerIndex = (int)carCount;
			cars[carCount++] = c;
		}
	}

	if (playerIndex >= 0) {
		LearnFrame(cars[playerIndex], dt);
		playerSpeed = cars[playerIndex].speed;
	} else if (feed && dt == 0) {
		// paused: keep the frame and the cloud as they are
		return;
	}

	// Wetness from the field's tyres: full wets count fully, inters a bit over half
	if (playerIndex >= 0) {
		float sum = 0;
		int known = 0;
		for (uint32_t i = 0; i < carCount; i++) {
			if (!cars[i].known)
				continue;
			known++;
			sum += cars[i].wet == 2 ? 1.0f : cars[i].wet == 1 ? 0.55f : 0.0f;
		}
		if (known > 0) {
			float target = sum / (float)known;
			if (!haveWetness)
				wetness = target; // a race that starts wet sprays from the first metre
			else
				wetness += (target - wetness) * (1.0f - std::exp(-dt / 15.0f));
			haveWetness = true;
		}
	} else {
		wetness = std::max(0.0f, wetness - dt * 0.2f);
		haveWetness = false;
		haveFrame = haveFrame && feed != nullptr;
	}

	// New spray
	if (haveFrame && sprayScale > 0 && wetness > 0.02f) {
		for (uint32_t i = 0; i < carCount; i++) {
			if (Length(FromArray(cars[i].pos) - eye) < kSprayRange)
				Emit(cars[i], dt);
		}
	} else {
		memset(emitDebt, 0, sizeof(emitDebt));
	}

	// The cloud: slowed by the air, settling a little, and the player's eyes inside it make the veil
	Vec3 worldUp = Axis(upAxis);
	float drag = std::exp(-1.3f * dt);
	float inCloud = 0;
	for (size_t i = 0; i < mist.size();) {
		Mist& m = mist[i];
		m.age += dt;
		if (m.age >= m.life) {
			m = mist.back();
			mist.pop_back();
			continue;
		}
		m.vel = m.vel * drag - worldUp * (0.2f * dt);
		m.pos = m.pos + m.vel * dt;
		m.angle += m.spin * dt;
		if (haveFrame) {
			float t = m.age / m.life;
			float size = m.size0 + (m.size1 - m.size0) * std::sqrt(t);
			float d = Length(m.pos - eye);
			if (d < size)
				inCloud += m.alpha * (1.0f - d / size) * (1.0f - t);
		}
		i++;
	}
	// The haze inside a cloud: capped, so the track ahead always shows through
	float veilTarget = std::min(0.3f * std::max(sprayScale, 0.35f) / 0.35f, 1.0f - std::exp(-inCloud * 0.15f));
	veil += (veilTarget - veil) * (1.0f - std::exp(-dt * 6.0f));
	if (!haveFrame)
		veil = std::max(0.0f, veil - dt);

	// Drops on the visor: rain on a wet track, many more inside another car's spray, more at speed
	float hitRate = 0;
	if (dropsOn && haveFrame)
		hitRate = (wetness * 5.0f + veil * 90.0f) * (0.35f + std::min(playerSpeed / 50.0f, 1.3f));
	UpdateBeads(dt, playerIndex >= 0 ? playerSpeed : 0.0f, hitRate);
}

void Sim::LearnFrame(const Car& player, float dt)
{
	// Up: the axis the car moves along least, averaged over a few seconds of driving
	if (player.speed > 15.0f && dt > 0) {
		float k = 1.0f - std::exp(-dt / 5.0f);
		for (int i = 0; i < 3; i++)
			axisMotion[i] += (std::fabs(player.vel[i]) - axisMotion[i]) * k;
		int least = 0;
		for (int i = 1; i < 3; i++)
			if (axisMotion[i] < axisMotion[least])
				least = i;
		if (axisMotion[least] < 0.5f * std::max({ axisMotion[0], axisMotion[1], axisMotion[2] }))
			upAxis = least;
	}

	Frame frame;
	if (!CarFrame(player, Axis(upAxis), 1.0f, frame)) {
		if (haveFrame) // standing still: keep the last frame, just move the eyes with the car
			eye = FromArray(player.pos) + up * kEyeUp + forward * kEyeAhead;
		return;
	}

	// Side: steering right turns the car towards right = up x forward when handedness is 1
	if (haveLastForward && player.speed > 15.0f && std::fabs(lastSteer) > 0.15f && dt > 0) {
		float yawRight = Dot(frame.forward - lastForward, frame.right) / dt;
		turnEvidence = std::clamp(turnEvidence + lastSteer * yawRight * dt, -20.0f, 20.0f);
		handedness = turnEvidence >= 0 ? 1.0f : -1.0f;
	}
	lastForward = frame.forward;
	haveLastForward = true;

	forward = frame.forward;
	up = frame.up;
	right = frame.right * handedness;
	eye = FromArray(player.pos) + up * kEyeUp + forward * kEyeAhead;
	haveFrame = true;
}

Vec3 Sim::ToTracking(const Vec3& world) const
{
	Vec3 rel = world - eye;
	return { Dot(rel, right), Dot(rel, up), -Dot(rel, forward) };
}

void Sim::Emit(const Car& car, float dt)
{
	Frame f;
	if (dt <= 0 || !CarFrame(car, Axis(upAxis), handedness, f))
		return;
	float pace = Smoothstep(10.0f, 45.0f, car.speed);
	if (pace <= 0)
		return;
	int index = (int)(&car - cars);

	// Wheels: ahead of the car's position, to its right, up; rear ones throw most
	struct Wheel {
		float ahead, side, up, rate;
		bool front;
	};
	static const Wheel wheels[4] = {
		{ -1.75f, -0.8f, 0.15f, 80.0f, false },
		{ -1.75f, 0.8f, 0.15f, 80.0f, false },
		{ 1.75f, -0.85f, 0.15f, 16.0f, true },
		{ 1.75f, 0.85f, 0.15f, 16.0f, true },
	};
	Vec3 pos = FromArray(car.pos), vel = FromArray(car.vel);
	for (int w = 0; w < 4; w++) {
		const Wheel& wheel = wheels[w];
		if (car.player && !wheel.front)
			continue; // behind the player's eyes
		float& debt = emitDebt[index][w];
		debt += wheel.rate * pace * wetness * sprayScale * dt;
		while (debt >= 1.0f && mist.size() < kMaxMist) {
			debt -= 1.0f;
			float outward = wheel.side > 0 ? 1.0f : -1.0f;
			Mist m;
			// spread over the frame, so a fast car leaves a trail instead of clumps
			m.pos = pos + f.forward * (wheel.ahead + Random(-0.3f, 0.3f)) + f.right * (wheel.side + Random(-0.15f, 0.15f)) + f.up * (wheel.up + Random(0.0f, 0.2f))
			    - vel * Random(0.0f, dt);
			if (wheel.front) {
				m.vel = vel * Random(0.5f, 0.75f) + f.right * (outward * Random(1.0f, 3.0f)) + f.up * Random(0.5f, 2.0f);
				m.life = Random(0.6f, 1.2f);
				m.size0 = Random(0.15f, 0.3f);
				m.size1 = Random(0.8f, 1.6f);
			} else {
				// a tall, wide cloud: real spray stands several metres high and wider than the car
				m.vel = vel * Random(0.55f, 0.8f) + f.right * (outward * Random(0.5f, 3.0f)) + f.up * Random(2.0f, 5.5f);
				m.life = Random(1.4f, 2.6f);
				m.size0 = Random(0.4f, 0.7f);
				m.size1 = Random(3.5f, 6.5f);
			}
			m.age = 0;
			m.alpha = Random(0.08f, 0.14f) * std::min(1.0f, 0.4f + wetness);
			m.spin = Random(-0.6f, 0.6f);
			m.angle = Random(0.0f, 2.0f * kPi);
			mist.push_back(m);
		}
		debt = std::min(debt, 4.0f);
	}
}

void Sim::UpdateBeads(float dt, float speed, float hitRate)
{
	beadDebt += hitRate * dt;
	while (beadDebt >= 1.0f) {
		beadDebt -= 1.0f;
		if (beads.size() >= kMaxBeads)
			continue;
		Bead b;
		b.x = Random(-0.9f, 0.9f) * kVisorHalfSize;
		b.y = Random(-0.7f, 0.9f) * kVisorHalfSize;
		b.vx = b.vy = 0;
		b.radius = Random(0.008f, 0.026f);
		b.age = 0;
		b.life = Random(4.0f, 9.0f);
		b.stretch = 1;
		b.angle = 0;
		beads.push_back(b);
	}
	beadDebt = std::min(beadDebt, 3.0f);

	// The airflow over the helmet pushes drops out to the edges and up at speed; slowly, big ones run down
	float air = std::clamp((speed - 8.0f) / 50.0f, 0.0f, 1.4f);
	for (size_t i = 0; i < beads.size();) {
		Bead& b = beads[i];
		b.age += dt * (1.0f + air * 1.5f);
		float tx = b.x * 1.6f * air + Random(-0.02f, 0.02f) * air;
		float ty = 0.35f * air - (b.radius > 0.018f ? 0.015f : 0.0f) * (1.0f - std::min(air, 1.0f));
		float k = 1.0f - std::exp(-dt * 4.0f);
		b.vx += (tx - b.vx) * k;
		b.vy += (ty - b.vy) * k;
		b.x += b.vx * dt;
		b.y += b.vy * dt;
		float moving = std::sqrt(b.vx * b.vx + b.vy * b.vy);
		b.stretch = 1.0f + std::min(moving * 5.0f, 3.0f);
		if (moving > 0.005f)
			b.angle = std::atan2(-b.vx, b.vy);
		if (b.age >= b.life || std::fabs(b.x) > kVisorHalfSize || std::fabs(b.y) > kVisorHalfSize) {
			b = beads.back();
			beads.pop_back();
			continue;
		}
		i++;
	}
}

void Sim::Build(std::vector<Sprite>& spray, std::vector<Sprite>& drops) const
{
	spray.clear();
	drops.clear();

	if (haveFrame) {
		for (const Mist& m : mist) {
			Vec3 at = ToTracking(m.pos);
			float d = Length(at);
			if (d < 0.8f || at.z > 2.0f) // too close to draw, or well behind the eyes
				continue;
			float t = m.age / m.life;
			float fadeIn = std::min(m.age / 0.12f, 1.0f);
			float fadeOut = 1.0f - t * t;
			float alpha = m.alpha * fadeIn * fadeOut * Smoothstep(1.0f, 4.0f, d);
			if (alpha < 0.003f)
				continue;
			float size = m.size0 + (m.size1 - m.size0) * std::sqrt(t);
			spray.push_back({ at, size, size, m.angle, alpha, kTilePuff });
		}
	}

	if (veil > 0.002f)
		drops.push_back({ {}, kVisorHalfSize, kVisorHalfSize, 0.0f, veil, kTileFlat });
	for (const Bead& b : beads) {
		// Landing: a quick splat, wider and stronger, that settles into a bead within a tenth of a second
		float splat = std::exp(-b.age / 0.05f);
		float alpha = std::min(1.0f, 0.85f + 0.4f * splat) * (1.0f - Smoothstep(b.life - 1.0f, b.life, b.age));
		float r = b.radius * (1.0f + 0.7f * splat);
		drops.push_back({ { b.x, b.y, 0.0f }, r, r * b.stretch, b.angle, alpha, kTileDrop });
	}
}

} // namespace Rain
