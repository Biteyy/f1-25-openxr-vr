/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Swofty engine plugin for the OpenXR VR addon. It holds the addon's settings (changed in the launcher, on the
 * addon's page in the game's Settings menu, or in ADDON SETTINGS) for openvr_api.dll, which asks for them through
 * OpenXrVrVisor and OpenXrVrRenderScale, and applies the "vr_graphics" preset to the game's VR graphics file.
 * No hooks, no game memory.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "swofty_api.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static const SwoftyApi* swofty;

static SRWLOCK lock = SRWLOCK_INIT;
static char visor[64] = "clear";
static unsigned int generation = 1; // bumped on every visor change, so the reader only reloads when it moves
static float render_scale = 0.0f; // per axis; 0: opencomposite.ini's
static int view_window = 0; // the VR view window for screen sharing

/* ---- VR graphics presets ----------------------------------------------------------------------------------------
 * Whole elements of hardware_settings_config_vr.xml, as the game itself writes them. "performance" is the low set
 * the game wrote on 2026-11-10 for a Quest 3 on an RTX 4060 Ti; "quality" is its high set. Only detail settings:
 * resolution, anti-aliasing, dynamic resolution, textures and the headset are left to the game's own menus. */
typedef struct {
	const char* tag; // "<tag " as it opens the element
	const char* performance;
	const char* quality;
} Element;

static const Element elements[] = {
	{ "<ssao ", "<ssao enabled=\"false\" algorithm=\"0\" />", "<ssao enabled=\"true\" algorithm=\"1\" />" },
	{ "<ssrt ", "<ssrt enabled=\"false\" quality=\"0\" />", "<ssrt enabled=\"true\" quality=\"3\" />" },
	{ "<anisotropic_filter ", "<anisotropic_filter level=\"4\" />", "<anisotropic_filter level=\"8\" />" },
	{ "<lighting ",
	    "<lighting quality=\"0\" volumetricFog=\"false\" volumetricFogMapWidth=\"256\" volumetricFogMapHeight=\"128\" />",
	    "<lighting quality=\"3\" volumetricFog=\"true\" volumetricFogMapWidth=\"1920\" volumetricFogMapHeight=\"1080\" />" },
	{ "<postprocess ",
	    "<postprocess motionBlur=\"false\" godRays=\"false\" depthOfField=\"false\" lensStreak=\"false\" lensFlare=\"false\" bloom=\"false\" motionBlurV2=\"false\" />",
	    "<postprocess motionBlur=\"false\" godRays=\"false\" depthOfField=\"true\" lensStreak=\"true\" lensFlare=\"true\" bloom=\"false\" motionBlurV2=\"false\" />" },
	{ "<shadows ",
	    "<shadows enabled=\"true\" skyShadowMapSize=\"512\" skyShadowCascadeCount=\"2\" skyShadowLowLod=\"true\" worldShadowMapSize=\"512\" skyShadowMap32Bits=\"false\" nightShadowMapSize=\"512\" nightShadowCharacterCast=\"false\" nightShadowSourceCount=\"1\" sampling=\"0\" spotShadowMapSize=\"512\" spotShadowSampling=\"0\" crowdInShadows=\"false\" />",
	    "<shadows enabled=\"true\" skyShadowMapSize=\"2048\" skyShadowCascadeCount=\"4\" skyShadowLowLod=\"false\" worldShadowMapSize=\"4096\" skyShadowMap32Bits=\"true\" nightShadowMapSize=\"4096\" nightShadowCharacterCast=\"true\" nightShadowSourceCount=\"4\" sampling=\"2\" spotShadowMapSize=\"2048\" spotShadowSampling=\"2\" crowdInShadows=\"true\" />" },
	{ "<particles ", "<particles enabled=\"true\" distanceScale=\"0.25\" rate=\"0.25\" high=\"false\" />",
	    "<particles enabled=\"true\" distanceScale=\"1.0\" rate=\"1.0\" high=\"true\" />" },
	{ "<crowd ", "<crowd distanceScale=\"0.25\" billboardDistance=\"250\" />", "<crowd distanceScale=\"0.4\" billboardDistance=\"3000\" />" },
	{ "<vehicle_reflections ",
	    "<vehicle_reflections envMapScale=\"0.25\" envMapUpdateMode=\"alternate\" envMapRenderMode=\"cars\" cubeMapEnabled=\"false\" cubeMapScale=\"0.25\" cubeMapUpdateMode=\"oneFacePerFrame\" />",
	    "<vehicle_reflections envMapScale=\"2.0\" envMapUpdateMode=\"alternate\" envMapRenderMode=\"carsAndCharacters\" cubeMapEnabled=\"true\" cubeMapScale=\"1.0\" cubeMapUpdateMode=\"oddsThenEvens\" />" },
	{ "<mirrors ",
	    "<mirrors mirrorsUpdateMode=\"alternate\" mirrorsRenderMode=\"cars\" treesInMirrors=\"false\" crowdInMirrors=\"false\" highDetailMirrors=\"false\" highDetailTyresAndCar=\"false\" particlesInMirrors=\"false\" />",
	    "<mirrors mirrorsUpdateMode=\"alternate\" mirrorsRenderMode=\"carsAndCharacters\" treesInMirrors=\"true\" crowdInMirrors=\"true\" highDetailMirrors=\"true\" highDetailTyresAndCar=\"true\" particlesInMirrors=\"true\" />" },
	{ "<weather_effects ",
	    "<weather_effects rainSplashes=\"true\" rainSheets=\"false\" rainBeads=\"false\" rainHigh=\"false\" trackHeatHaze=\"false\" planarReflectionsEnabled=\"false\" planarReflectionsHQEnabled=\"false\" planarReflectionsRangeScale=\"0.5\" planarReflectionsRTScale=\"0.25\" planarReflectionsMipTailBlur=\"true\" proceduralCloudRTScale=\"0.5\" proceduralCloudTiling=\"4\" proceduralCloudQuality=\"0\" />",
	    "<weather_effects rainSplashes=\"true\" rainSheets=\"true\" rainBeads=\"true\" rainHigh=\"true\" trackHeatHaze=\"true\" planarReflectionsEnabled=\"true\" planarReflectionsHQEnabled=\"true\" planarReflectionsRangeScale=\"3.0\" planarReflectionsRTScale=\"1.0\" planarReflectionsMipTailBlur=\"true\" proceduralCloudRTScale=\"4.0\" proceduralCloudTiling=\"7\" proceduralCloudQuality=\"1\" />" },
	{ "<ground_cover ", "<ground_cover enabled=\"false\" grassFlattening=\"false\" drawDistance=\"30.0\" meshes=\"false\" />",
	    "<ground_cover enabled=\"true\" grassFlattening=\"true\" drawDistance=\"120.0\" meshes=\"true\" />" },
	{ "<trees ", "<trees useLOD0=\"false\" distanceScale=\"0.5\" />", "<trees useLOD0=\"true\" distanceScale=\"0.2\" />" },
	{ "<hair ", "<hair enabled=\"false\" />", "<hair enabled=\"true\" />" },
	{ "<skidmarks ", "<skidmarks enabled=\"true\" parallax=\"false\" />", "<skidmarks enabled=\"true\" parallax=\"true\" />" },
};

/* The game's VR graphics file, or 0 if the Documents folder can't be found */
static int graphics_path(wchar_t* out, size_t size, const wchar_t* suffix)
{
	PWSTR docs = NULL;
	if (FAILED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs)))
		return 0;
	int n = _snwprintf_s(out, size, _TRUNCATE, L"%s\\My Games\\F1 25\\hardwaresettings\\hardware_settings_config_vr%s.xml", docs, suffix);
	CoTaskMemFree(docs);
	return n > 0;
}

static char* read_file(const wchar_t* path, size_t* size)
{
	FILE* f = NULL;
	if (_wfopen_s(&f, path, L"rb") || !f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* data = n > 0 && n < (1 << 20) ? malloc((size_t)n + 1) : NULL;
	if (data && fread(data, 1, (size_t)n, f) == (size_t)n) {
		data[n] = 0;
		*size = (size_t)n;
	} else {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

/* Rewrites the file's detail elements with the preset's ("performance" or "quality"); anything else leaves it be. */
static void apply_graphics(const char* preset)
{
	int quality = strcmp(preset, "quality") == 0;
	if (!quality && strcmp(preset, "performance") != 0)
		return;

	wchar_t path[MAX_PATH], backup[MAX_PATH], temp[MAX_PATH];
	if (!graphics_path(path, MAX_PATH, L"") || !graphics_path(backup, MAX_PATH, L".before-openxr-vr") || !graphics_path(temp, MAX_PATH, L".openxr-vr-tmp")) {
		swofty->log("openxr_vr_settings: no Documents folder, so the VR graphics preset isn't applied");
		return;
	}
	DWORD attributes = GetFileAttributesW(path);
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		swofty->log("openxr_vr_settings: the game hasn't written its VR graphics file yet; the preset applies once it has (start VR once, then restart)");
		return;
	}
	if (attributes & FILE_ATTRIBUTE_READONLY) {
		swofty->log("openxr_vr_settings: the VR graphics file is read-only, so the preset isn't applied");
		return;
	}

	size_t size = 0;
	char* old = read_file(path, &size);
	if (!old) {
		swofty->log("openxr_vr_settings: can't read the VR graphics file");
		return;
	}

	// The new file: each element found is swapped whole for the preset's; elements the file lacks are left out
	size_t cap = size + 8192;
	char* out = malloc(cap);
	size_t len = 0;
	const char* at = old;
	int changed = 0;
	if (!out) {
		free(old);
		return;
	}
	for (;;) {
		// The next element to replace, in file order
		const char* next = NULL;
		const Element* which = NULL;
		for (size_t i = 0; i < sizeof(elements) / sizeof(elements[0]); i++) {
			const char* p = strstr(at, elements[i].tag);
			if (p && (!next || p < next)) {
				next = p;
				which = &elements[i];
			}
		}
		const char* end = next ? strstr(next, "/>") : NULL;
		if (!end)
			break;
		end += 2;
		const char* text = quality ? which->quality : which->performance;
		size_t keep = (size_t)(next - at), add = strlen(text);
		if (len + keep + add >= cap)
			break; // can't happen for a file the game wrote; leave the rest as it is
		memcpy(out + len, at, keep);
		len += keep;
		memcpy(out + len, text, add);
		len += add;
		changed |= (size_t)(end - next) != add || memcmp(next, text, add) != 0;
		at = end;
	}
	size_t rest = strlen(at);
	if (len + rest < cap) {
		memcpy(out + len, at, rest);
		len += rest;
	} else {
		changed = 0;
	}

	if (changed) {
		CopyFileW(path, backup, TRUE); // the file as it was before this addon first changed it; kept from then on
		FILE* f = NULL;
		int ok = !_wfopen_s(&f, temp, L"wb") && f && fwrite(out, 1, len, f) == len;
		if (f)
			ok = !fclose(f) && ok;
		ok = ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
		if (!ok)
			DeleteFileW(temp);
		swofty->log(ok ? "openxr_vr_settings: VR graphics set to %s; it applies from the next game start" : "openxr_vr_settings: can't write the VR graphics file (%s)", preset);
	} else {
		swofty->log("openxr_vr_settings: VR graphics already %s", preset);
	}
	free(out);
	free(old);
}

/* ---- Settings ---------------------------------------------------------------------------------------------------- */

/* ---- The player's driver's visor ---------------------------------------------------------------------------------
 * "driver": the visor of the helmet the player's driver wears, from telemetry (driver id and team) and the table of
 * stock helmets' visor colours. The visor id handed on is then "rgba_RRGGBBAA", which openvr_api.dll draws as that
 * colour. Drivers the table lacks (your own driver, mod drivers, helmets with the game's generic visor) get "clear". */
#include "driver_visors.h"

static int follow_driver; // the setting is "driver"
static char driver_visor[64] = "clear"; // what that resolves to now
static ULONGLONG next_driver_look;

static const DriverVisor* find_driver_visor(unsigned driver, unsigned season)
{
	// The driver's helmet for that season, else the nearest season's
	static const int order[3][3] = { { 2025, 2026, 2024 }, { 2026, 2025, 2024 }, { 2024, 2025, 2026 } };
	const int* seasons = order[season == 2026 ? 1 : season == 2024 ? 2 : 0];
	for (int i = 0; i < 3; i++) {
		for (size_t j = 0; j < sizeof(driver_visors) / sizeof(driver_visors[0]); j++) {
			if (driver_visors[j].driver == driver && driver_visors[j].season == seasons[i])
				return &driver_visors[j];
		}
	}
	return NULL;
}

/* Looks the player's driver up again, at most once a second; called with the lock held exclusively */
static void update_driver_visor(void)
{
	ULONGLONG now = GetTickCount64();
	if (now < next_driver_look)
		return;
	next_driver_look = now + 1000;

	const SwoftyTelemetry* tel = swofty && swofty->version >= 5 ? swofty->telemetry : NULL;
	if (!tel || !tel->available || !tel->available())
		return; // no session: keep the last driver's visor
	SwoftySessionState session = { 0 };
	SwoftyParticipant me = { 0 };
	if (tel->session(&session) != 0 || tel->participant(session.player_index, &me) != 0 || me.version < 2)
		return;

	// 2026 teams have the stock 2025 team id + 476
	const DriverVisor* found = find_driver_visor(me.driver_id, me.team >= 476 ? 2026 : 2025);
	char id[64];
	if (found)
		sprintf_s(id, sizeof(id), "rgba_%02x%02x%02x%02x", found->rgba[0], found->rgba[1], found->rgba[2], found->rgba[3]);
	else
		strcpy_s(id, sizeof(id), "clear");
	if (strcmp(id, driver_visor) != 0) {
		strcpy_s(driver_visor, sizeof(driver_visor), id);
		generation++;
		swofty->log("openxr_vr_settings: driver %u (team %u): visor %s", me.driver_id, me.team, id);
	}
}

static void store_visor(const char* value)
{
	AcquireSRWLockExclusive(&lock);
	if (value && strcmp(value, visor) != 0) {
		strncpy_s(visor, sizeof(visor), value, _TRUNCATE);
		follow_driver = strcmp(visor, "driver") == 0;
		next_driver_look = 0;
		generation++;
	}
	ReleaseSRWLockExclusive(&lock);
}

static void store_render_scale(const char* value)
{
	// "default" (opencomposite.ini) or a percentage of the headset's recommended size per axis, eg "120"
	float scale = 0.0f;
	if (value && strcmp(value, "default") != 0) {
		int percent = atoi(value);
		if (percent >= 50 && percent <= 200)
			scale = (float)percent / 100.0f;
	}
	AcquireSRWLockExclusive(&lock);
	render_scale = scale;
	ReleaseSRWLockExclusive(&lock);
}

static void on_change(const char* id, const SwoftySettingValue* value, void* user)
{
	(void)user;
	if (id && value && value->type == SWOFTY_SETTING_TOGGLE && strcmp(id, "view_window") == 0) {
		AcquireSRWLockExclusive(&lock);
		view_window = value->toggle != 0;
		ReleaseSRWLockExclusive(&lock);
		return;
	}
	if (!id || !value || value->type != SWOFTY_SETTING_CHOICE || !value->choice)
		return;
	if (strcmp(id, "visor") == 0)
		store_visor(value->choice);
	else if (strcmp(id, "render_scale") == 0)
		store_render_scale(value->choice);
	else if (strcmp(id, "vr_graphics") == 0)
		apply_graphics(value->choice);
}

/* The current visor id (eg "dark_smoke", or "off") copied into out, and a number that changes whenever it does. */
__declspec(dllexport) unsigned int OpenXrVrVisor(char* out, unsigned int size)
{
	AcquireSRWLockExclusive(&lock);
	if (follow_driver)
		update_driver_visor();
	strncpy_s(out, size, follow_driver ? driver_visor : visor, _TRUNCATE);
	unsigned int gen = generation;
	ReleaseSRWLockExclusive(&lock);
	return gen;
}

/* The render resolution scale per axis, or 0 for opencomposite.ini's. Read when VR starts. */
__declspec(dllexport) float OpenXrVrRenderScale(void)
{
	AcquireSRWLockShared(&lock);
	float scale = render_scale;
	ReleaseSRWLockShared(&lock);
	return scale;
}

/* Whether the VR view window is on (1) or off (0). Polled every frame. */
__declspec(dllexport) int OpenXrVrViewWindow(void)
{
	AcquireSRWLockShared(&lock);
	int on = view_window;
	ReleaseSRWLockShared(&lock);
	return on;
}

__declspec(dllexport) const uint32_t SwoftyAddonApi = SWOFTY_API_VERSION;

static const char* read_choice(const char* id)
{
	SwoftySettingValue value = { 0 };
	return swofty->setting(id, SWOFTY_SETTING_CHOICE, &value) ? value.choice : NULL;
}

void settings_tile_init(const SwoftyApi* api); /* settings_tile.c */

__declspec(dllexport) int SwoftyAddonInit(const SwoftyApi* api)
{
	swofty = api;
	settings_tile_init(api);
	if (api->version < 3 || !api->setting || !api->watch_setting) {
		api->log("openxr_vr_settings: this Swofty core has no settings API; the defaults apply");
		return 1;
	}
	static const char* ids[] = { "visor", "render_scale", "vr_graphics" };
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		const char* choice = read_choice(ids[i]);
		if (choice) {
			SwoftySettingValue value = { 0 };
			value.type = SWOFTY_SETTING_CHOICE;
			value.choice = choice;
			on_change(ids[i], &value, NULL);
		}
		if (!api->watch_setting(ids[i], on_change, NULL))
			api->log("openxr_vr_settings: can't watch %s; changes apply from the next start", ids[i]);
	}
	SwoftySettingValue toggle = { 0 };
	if (api->setting("view_window", SWOFTY_SETTING_TOGGLE, &toggle)) {
		toggle.type = SWOFTY_SETTING_TOGGLE;
		on_change("view_window", &toggle, NULL);
	}
	if (!api->watch_setting("view_window", on_change, NULL))
		api->log("openxr_vr_settings: can't watch view_window; changes apply from the next start");
	api->log("openxr_vr_settings: visor %s, render scale %.2f (0: opencomposite.ini), view window %s", visor, render_scale, view_window ? "on" : "off");
	return 1;
}
