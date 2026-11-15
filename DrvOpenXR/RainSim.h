#pragma once

/**
 * Tyre spray and visor rain drops for F1 25 in VR, which draws neither: the game turns its spray off in VR, and
 * rewrites its VR settings with its camera rain beads off.
 *
 * RainSim is the simulation, with no graphics or OpenXR in it: each frame it takes the cars from the Swofty addon
 * plugin (world position and velocity, tyres) and gives back sprites for RainDraw to draw:
 *   - spray: soft mist puffs thrown up behind each car's tyres on a wet track, in the VR tracking space (OpenXR:
 *     x right, y up, -z ahead, origin at the driver's eyes), plus a veil when the player drives through a cloud;
 *   - drops: the veil (a haze while the player drives through a cloud) and water drops on the visor, in metres on a
 *     head-locked plane, blown about by the airflow at speed.
 *
 * The track's wetness is read from the tyres the field is on (wets, inters), since the game's weather isn't
 * available to the plugin. The world's up axis and handedness aren't documented either, so they are learned from
 * the player's car: up is the axis it hardly ever moves along, and the side is set by which way it turns when the
 * player steers right.
 */

#include <cstdint>
#include <random>
#include <vector>

namespace Rain {

/** The plugin's answer (bitey_openxr_vr.dll, OpenXrVrRain), the layout both sides agree on. */
struct Car {
	float pos[3], vel[3]; // world, metres and metres per second
	float speed; // metres per second
	uint8_t wet; // tyres: 0 slicks or unknown, 1 intermediates, 2 full wets
	uint8_t player; // the car the cockpit camera is in
	uint8_t known; // the tyre is known
	uint8_t pad;
	float lap; // metres round the lap, or -1 when unknown
};

static const uint32_t kMaxCars = 32;

struct Feed {
	uint32_t size; // sizeof(Feed), set by the caller
	uint32_t count; // cars filled
	uint64_t seq; // the live snapshot's counter: changes every game frame
	float steer; // the player's steering, -1 (left) to 1 (right)
	uint32_t spray; // setting: 0 off, 1 light, 2 normal, 3 heavy
	uint32_t drops; // setting: visor drops on
	float trackLength; // metres, or 0 when unknown
	Car cars[kMaxCars];
};
static_assert(sizeof(Car) == 36 && sizeof(Feed) == 32 + 36 * kMaxCars, "Rain::Feed must match the plugin's RainFeed");

struct Vec3 {
	float x = 0, y = 0, z = 0;
};

/** One sprite: a camera-facing quad (spray) or a flat one on the head-locked plane (the veil). */
struct Sprite {
	Vec3 pos; // tracking space (spray), or head-locked plane x, y with z unused (veil)
	float halfWidth, halfHeight; // metres
	float angle; // rotation in the sprite's plane, radians
	float alpha; // 0..1, multiplies the tile's coverage
	uint32_t tile; // RainDraw tile: kTilePuff, kTileDrop or kTileFlat
};

static const uint32_t kTilePuff = 0, kTileDrop = 1, kTileFlat = 2;

/** The visor plane: how far ahead of the eyes it is drawn, and its half size there (metres) */
static const float kVisorDistance = 0.6f;
static const float kVisorHalfSize = 1.0f;

class Sim {
public:
	Sim();

	/**
	 * Advances by dt seconds (clamped) with the plugin's latest feed; haveFeed false (no session, plugin missing)
	 * lets everything fade out. seconds: a steady clock, to tell how old the feed's snapshot is.
	 */
	void Update(const Feed* feed, double seconds, float dt);

	/** Builds this frame's sprites: spray in the tracking space, the veil and drops on the head-locked plane */
	void Build(std::vector<Sprite>& spray, std::vector<Sprite>& drops) const;

	/** Whether anything would be drawn */
	bool Active() const { return !mist.empty() || !beads.empty() || veil > 0.002f; }

	/** Track wetness 0..1, for the log */
	float Wetness() const { return wetness; }

private:
	struct Mist {
		Vec3 pos, vel; // world
		float age, life, size0, size1, spin, angle, alpha;
		float lap; // where round the lap it was thrown up (-1 unknown)
	};
	struct Bead {
		float x, y; // visor plane, metres
		float vx, vy;
		float radius, age, life, stretch, angle;
	};

	void LearnFrame(const Car& player, float dt);
	Vec3 ToTracking(const Vec3& world) const;
	bool OnPlayersStretch(float lap, float straightDistance) const;
	void Emit(const Car& car, float dt);
	void UpdateBeads(float dt, float playerSpeed, float hitRate);

	std::mt19937 rng;
	float Random(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }

	std::vector<Mist> mist;
	std::vector<Bead> beads;

	// The feed's cars, moved on to now by their velocity (the snapshot can be a frame old)
	Car cars[kMaxCars] = {};
	uint32_t carCount = 0;
	int playerIndex = -1;
	uint64_t lastSeq = 0;
	double seqSeconds = 0;
	bool haveFrame = false;

	// The player's frame in world space: eyes, forward, up, right
	Vec3 eye, forward, up, right;

	// Learned world conventions
	int upAxis = 1;
	float axisMotion[3] = { 0, 0, 0 };
	float handedness = 1.0f; // sign of right = up x forward
	float turnEvidence = 0;
	float lastSteer = 0; // the player's steering this frame
	Vec3 lastForward;
	bool haveLastForward = false;

	float wetness = 0; // 0..1, follows the field's tyres slowly
	bool haveWetness = false;
	float sprayScale = 1.0f; // setting
	bool dropsOn = true; // setting
	float veil = 0; // alpha of the cloud the player is in
	float playerSpeed = 0;
	float playerLap = -1, trackLength = 0;
	float emitDebt[kMaxCars][4] = {};
	float beadDebt = 0;
};

} // namespace Rain
