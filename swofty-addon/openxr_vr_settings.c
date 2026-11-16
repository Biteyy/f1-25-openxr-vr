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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "swofty_api.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

static const SwoftyApi* swofty;

static SRWLOCK lock = SRWLOCK_INIT;
static char visor[64] = "clear"; // the visor type
static char visor_colour[32] = "blue"; // its colour, for the tinted and iridium types
static unsigned int generation = 1; // bumped on every visor change, so the reader only reloads when it moves
static float render_scale = 0.0f; // per axis; 0: opencomposite.ini's
static int view_window = 0; // the VR view window for screen sharing
static int rain_spray = 2; // tyre spray: 0 off, 1 light, 2 normal, 3 heavy
static int visor_drops = 1; // rain drops on the visor
static int starting = 1; // SwoftyAddonInit is reading the settings (not a change while the game runs)

/* tear_off.c: the tear-off button */
void tear_off_init(const SwoftyApi* api);
void tear_off_setting(const char* id, const char* choice, int at_start);
uint32_t tear_off_mode(void);
uint32_t tear_off_pulls(void);

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
 * "driver": the visor of the helmet the player's driver wears. The driver and team come from the loaded career (the
 * game's UDP telemetry, the other source, is off in VR); the game's database then gives the helmet: the line-up seat
 * of that driver in that team names a driver customisation, whose name is the helmet's (eg 2026-ferrari_lewis_hamilton).
 * driver_visors.h has the visor colour of every helmet in the game's files, read from each helmet's visor texture by
 * make_driver_visors.py. The visor id handed on is then "rgba_RRGGBBAA", which openvr_api.dll draws as that colour.
 * Helmets with the game's generic visor, and drivers it can't place (your own created driver), get "clear". */
#include "driver_visors.h"

static int follow_driver; // the setting is "driver"
static char driver_visor[64] = "clear"; // what that resolves to now
static ULONGLONG next_driver_look;
static uint32_t looked_driver = ~0u, looked_team = ~0u; // the career keys last looked up

static int visor_cmp(const void* key, const void* entry)
{
	return strcmp((const char*)key, ((const DriverVisor*)entry)->helmet);
}

static const DriverVisor* find_helmet_visor(const char* helmet)
{
	return (const DriverVisor*)bsearch(helmet, driver_visors, sizeof(driver_visors) / sizeof(driver_visors[0]), sizeof(driver_visors[0]), visor_cmp);
}

/* A text, enum or reference value of the game's database copied into out ("" and 0 when missing) */
static int db_text(const char* key, char* out, size_t cap)
{
	SwoftyDbValue v = { 0 };
	out[0] = 0;
	if (swofty->db->get(key, &v) != 0 || !v.s || !*v.s || (v.tag != SWOFTY_DB_STRING && v.tag != SWOFTY_DB_ENUM && v.tag != SWOFTY_DB_ID))
		return 0;
	strncpy_s(out, cap, v.s, _TRUNCATE);
	return 1;
}

static int cell_text(const SwoftyDbCursor* c, const char* field, char* out, size_t cap)
{
	SwoftyDbValue v = { 0 };
	out[0] = 0;
	if (swofty->db->cell(c, field, &v) != 0 || !v.s || !*v.s)
		return 0;
	strncpy_s(out, cap, v.s, _TRUNCATE);
	return 1;
}

/* The label of the row of table whose stat_id is stat */
static int label_of(const char* table, uint32_t stat, char* out, size_t cap)
{
	SwoftyDbCursor c;
	memset(&c, 0, sizeof(c));
	out[0] = 0;
	if (swofty->db->open(table, &c) != 0)
		return 0;
	do {
		SwoftyDbValue v = { 0 };
		const char* name = swofty->db->row_name(&c);
		if (name && swofty->db->cell(&c, "stat_id", &v) == 0 && v.i == (int64_t)stat) {
			strncpy_s(out, cap, name, _TRUNCATE);
			return 1;
		}
	} while (swofty->db->step(&c) == 0);
	return 0;
}

static int year_of(const char* key)
{
	return (strlen(key) > 5 && key[4] == '-') ? atoi(key) : 0;
}

/* The driver customisation (= helmet) the driver wears: his seat in the team's line-up, else his entrant of the
 * team's season (a team label ending _26 is 2026), else his latest */
static int helmet_of(const char* driver, const char* team, char* out, size_t cap)
{
	char key[192], text[96], ref[96];
	out[0] = 0;
	if (*team) {
		int64_t lists = 0;
		swofty->db->int_at("entrants.entrant_lists._entrant_list_instancecount_", &lists);
		for (int64_t l = 0; l < lists && l < 1000; l++) {
			int64_t seats = 0;
			sprintf_s(key, sizeof(key), "entrants.entrant_lists.entrant_list_%lld._entrant_instancecount_", l);
			if (swofty->db->int_at(key, &seats) != 0)
				continue;
			for (int64_t s = 0; s < seats && s < 100; s++) {
				sprintf_s(key, sizeof(key), "entrants.entrant_lists.entrant_list_%lld.entrant_%lld.team", l, s);
				if (!db_text(key, text, sizeof(text)) || strcmp(text, team) != 0)
					continue;
				// The seat names its driver itself, or through a shared entrant
				sprintf_s(key, sizeof(key), "entrants.entrant_lists.entrant_list_%lld.entrant_%lld.driver", l, s);
				if (db_text(key, text, sizeof(text)) && strcmp(text, driver) == 0) {
					sprintf_s(key, sizeof(key), "entrants.entrant_lists.entrant_list_%lld.entrant_%lld.driver_customisation", l, s);
					if (db_text(key, out, cap))
						return 1;
				}
				sprintf_s(key, sizeof(key), "entrants.entrant_lists.entrant_list_%lld.entrant_%lld.entrant_reference", l, s);
				if (!db_text(key, ref, sizeof(ref)))
					continue;
				SwoftyDbCursor c;
				memset(&c, 0, sizeof(c));
				if (swofty->db->row("entrants.entrant_lists.entrants", ref, &c) != 0)
					continue;
				if (cell_text(&c, "driver", text, sizeof(text)) && strcmp(text, driver) == 0 && cell_text(&c, "driver_customisation", out, cap))
					return 1;
			}
		}
	}

	int want = 0;
	size_t n = strlen(team);
	if (n > 3 && team[n - 3] == '_' && team[n - 2] >= '0' && team[n - 2] <= '9' && team[n - 1] >= '0' && team[n - 1] <= '9')
		want = 2000 + atoi(team + n - 2);
	SwoftyDbCursor c;
	memset(&c, 0, sizeof(c));
	if (swofty->db->open("entrants.entrant_lists.entrants", &c) != 0)
		return 0;
	int best = -1;
	do {
		if (!cell_text(&c, "driver", text, sizeof(text)) || strcmp(text, driver) != 0 || !cell_text(&c, "driver_customisation", text, sizeof(text)))
			continue;
		int y = year_of(text);
		int score = (want && y == want ? 10000 : 0) + y;
		if (score > best) {
			best = score;
			strncpy_s(out, cap, text, _TRUNCATE);
		}
	} while (swofty->db->step(&c) == 0);
	return best >= 0;
}

/* A career's packed key: the stat id in the high half (as career events give a driver, "stat id << 16") */
static uint32_t unpack_key(uint32_t key)
{
	return key > 0xffff ? key >> 16 : key;
}

/* Looks the player's driver up again, at most once a second; called with the lock held exclusively */
static void update_driver_visor(void)
{
	ULONGLONG now = GetTickCount64();
	if (now < next_driver_look || !swofty || swofty->version < 5)
		return;
	next_driver_look = now + 1000;

	uint32_t driver_key = 0, team_key = 0, driver_stat = 0, team_stat = 0;
	const char* source = NULL;
	const SwoftyCareer* career = swofty->career;
	SwoftyCareerState state;
	memset(&state, 0, sizeof(state));
	if (career && career->available && career->available() && career->state && career->state(&state) == 0 && state.loaded && state.player_driver) {
		driver_key = state.player_driver;
		team_key = state.player_team;
		driver_stat = unpack_key(driver_key);
		team_stat = team_key ? unpack_key(team_key) : 0;
		source = "career";
	} else {
		// Outside a career: the game's UDP telemetry, when it's on (it's off in VR)
		const SwoftyTelemetry* tel = swofty->telemetry;
		SwoftySessionState session = { 0 };
		SwoftyParticipant me = { 0 };
		if (tel && tel->available && tel->available() && tel->session(&session) == 0 && tel->participant(session.player_index, &me) == 0 && me.version >= 2 && me.name[0]) {
			driver_key = driver_stat = me.driver_id;
			source = "telemetry";
		}
	}
	if (!source || (driver_key == looked_driver && team_key == looked_team))
		return; // no driver known yet (keep the last visor), or the same one as last time
	looked_driver = driver_key;
	looked_team = team_key;

	char driver[96] = "", team[96] = "", helmet[96] = "", id[64] = "clear";
	if (swofty->db && swofty->db->available && swofty->db->available()) {
		if (label_of("entrants.drivers", driver_stat, driver, sizeof(driver))) {
			if (team_stat)
				label_of("entrants.teams", team_stat, team, sizeof(team));
			helmet_of(driver, team, helmet, sizeof(helmet));
		}
	} else {
		looked_driver = looked_team = ~0u; // try again once the database is up
	}
	const DriverVisor* found = *helmet ? find_helmet_visor(helmet) : NULL;
	if (found)
		sprintf_s(id, sizeof(id), "rgba_%02x%02x%02x%02x", found->rgba[0], found->rgba[1], found->rgba[2], found->rgba[3]);
	swofty->log("openxr_vr_settings: driver from %s: key 0x%x (stat %u, %s), team key 0x%x (stat %u, %s): helmet %s, visor %s%s", source,
	    driver_key, driver_stat, *driver ? driver : "not in the drivers table", team_key, team_stat, *team ? team : "-", *helmet ? helmet : "not found", id,
	    *helmet && !found ? " (the helmet has the game's generic visor)" : "");
	if (strcmp(id, driver_visor) != 0) {
		strcpy_s(driver_visor, sizeof(driver_visor), id);
		generation++;
	}
}
static void store_visor(const char* value)
{
	AcquireSRWLockExclusive(&lock);
	if (value && strcmp(value, visor) != 0) {
		strncpy_s(visor, sizeof(visor), value, _TRUNCATE);
		follow_driver = strcmp(visor, "driver") == 0;
		next_driver_look = 0;
		looked_driver = looked_team = ~0u;
		generation++;
	}
	ReleaseSRWLockExclusive(&lock);
}

static void store_visor_colour(const char* value)
{
	AcquireSRWLockExclusive(&lock);
	if (value && strcmp(value, visor_colour) != 0) {
		strncpy_s(visor_colour, sizeof(visor_colour), value, _TRUNCATE);
		generation++;
	}
	ReleaseSRWLockExclusive(&lock);
}

/* Whether the visor type takes the colour setting */
static int coloured(const char* type)
{
	return strcmp(type, "tinted") == 0 || strcmp(type, "iridium") == 0 || strcmp(type, "iridium_dark") == 0;
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

static void store_rain_spray(const char* value)
{
	static const char* levels[] = { "off", "light", "normal", "heavy" };
	for (int i = 0; i < 4; i++) {
		if (value && strcmp(value, levels[i]) == 0) {
			AcquireSRWLockExclusive(&lock);
			rain_spray = i;
			ReleaseSRWLockExclusive(&lock);
		}
	}
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
	if (id && value && value->type == SWOFTY_SETTING_TOGGLE && strcmp(id, "visor_drops") == 0) {
		AcquireSRWLockExclusive(&lock);
		visor_drops = value->toggle != 0;
		ReleaseSRWLockExclusive(&lock);
		return;
	}

	if (!id || !value || value->type != SWOFTY_SETTING_CHOICE || !value->choice)
		return;
	if (strcmp(id, "visor") == 0)
		store_visor(value->choice);
	else if (strcmp(id, "visor_colour") == 0)
		store_visor_colour(value->choice);
	else if (strcmp(id, "render_scale") == 0)
		store_render_scale(value->choice);
	else if (strcmp(id, "vr_graphics") == 0)
		apply_graphics(value->choice);
	else if (strcmp(id, "rain_spray") == 0)
		store_rain_spray(value->choice);
	else if (strcmp(id, "tear_offs") == 0 || strcmp(id, "tear_off_key") == 0)
		tear_off_setting(id, value->choice, starting);
}

/* The current visor id (eg "dark_smoke", or "off") copied into out, and a number that changes whenever it does. */
__declspec(dllexport) unsigned int OpenXrVrVisor(char* out, unsigned int size)
{
	AcquireSRWLockExclusive(&lock);
	if (follow_driver)
		update_driver_visor();
	if (follow_driver)
		strncpy_s(out, size, driver_visor, _TRUNCATE);
	else if (coloured(visor))
		_snprintf_s(out, size, _TRUNCATE, "%s_%s", visor, visor_colour); /* eg "iridium_dark_ice_blue" */
	else
		strncpy_s(out, size, visor, _TRUNCATE);
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

/* ---- Rain: the cars around the player, for openvr_api.dll's tyre spray and visor drops ---------------------------
 * The game turns its tyre spray off in VR, so the DLL draws its own (DrvOpenXR/RainSim.h). It needs every car's world
 * position, velocity and tyres, from Swofty's live state (the core export SwoftyCoreLive, read from game memory, so
 * the game's UDP output can stay off). The layout is RainSim.h's Rain::Feed; keep the two the same. */
typedef struct {
	float pos[3], vel[3];
	float speed;
	uint8_t wet; // 0 slicks or unknown, 1 intermediates, 2 full wets
	uint8_t player;
	uint8_t known;
	uint8_t pad;
	float lap; // metres round the lap, or -1 when unknown
} RainCar;

typedef struct {
	uint32_t size;
	uint32_t count;
	uint64_t seq;
	float steer;
	uint32_t spray, drops;
	float track_length; // metres, 0 when unknown
	RainCar cars[32];
	uint32_t tear_offs; // 0 off, 1 automatic and the button, 2 the button only
	uint32_t tear_off_pulls; // counts the button's presses
} RainFeed;
_Static_assert(sizeof(RainCar) == 36 && sizeof(RainFeed) == 40 + 36 * 32, "RainFeed must match RainSim.h's Rain::Feed");

static const SwoftyLive* live;
static ULONGLONG next_live_look;
static int rain_logged;

/* The live table, looked up again every few seconds until the core has it; called with the lock held */
static const SwoftyLive* find_live(void)
{
	if (live || GetTickCount64() < next_live_look)
		return live;
	next_live_look = GetTickCount64() + 5000;
	HMODULE core = GetModuleHandleW(L"swofty_core.dll");
	SwoftyCoreLiveFn get = core ? (SwoftyCoreLiveFn)GetProcAddress(core, "SwoftyCoreLive") : NULL;
	const SwoftyLive* table = get ? get() : NULL;
	if (table && table->version >= 1 && table->available && table->session && table->car) {
		live = table;
		swofty->log("openxr_vr_settings: live state found (version %u), so VR tyre spray can follow the cars", table->version);
	} else if (!(rain_logged & 1)) {
		rain_logged |= 1;
		swofty->log("openxr_vr_settings: this Swofty core has no live state (SwoftyCoreLive), so no VR tyre spray");
	}
	return live;
}

/* Fills out (a RainFeed of size bytes); 1 while a session is live with cars in it, else 0. */
__declspec(dllexport) int OpenXrVrRain(void* out_, unsigned int size)
{
	if (!out_ || size < sizeof(RainFeed))
		return 0;
	RainFeed* out = (RainFeed*)out_;
	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);

	AcquireSRWLockExclusive(&lock);
	out->spray = (uint32_t)rain_spray;
	out->drops = (uint32_t)visor_drops;
	out->tear_offs = tear_off_mode();
	out->tear_off_pulls = tear_off_pulls();
	const SwoftyLive* table = (rain_spray || visor_drops) && swofty ? find_live() : NULL;
	ReleaseSRWLockExclusive(&lock);
	if (!table || !table->available())
		return 0;

	SwoftyLiveSession session;
	memset(&session, 0, sizeof(session));
	session.version = SWOFTY_LIVE_VERSION;
	session.size = sizeof(session);
	if (table->session(&session) != SWOFTY_ASSET_OK)
		return 0;
	int subject = session.subject >= 0 ? session.subject : session.player;
	if (session.have & SWOFTY_LIVE_SHAVE_TRACK_LENGTH)
		out->track_length = session.track_length_m;
	uint64_t frame = 0;
	if (table->seq && table->seq(&frame) == SWOFTY_ASSET_OK)
		out->seq = frame;

	int tyres = 0;
	for (uint32_t v = 0; v < SWOFTY_LIVE_MAX_CARS && out->count < 32; v++) {
		SwoftyLiveCar car;
		memset(&car, 0, sizeof(car));
		car.version = SWOFTY_LIVE_VERSION;
		car.size = sizeof(car);
		if (table->car(v, &car) != SWOFTY_ASSET_OK || !(car.have & SWOFTY_LIVE_HAVE_WORLD))
			continue;
		RainCar* c = &out->cars[out->count++];
		c->lap = (car.have & SWOFTY_LIVE_HAVE_PROGRESS) ? car.lap_m : -1.0f;
		memcpy(c->pos, car.world_pos, sizeof(c->pos));
		memcpy(c->vel, car.world_vel, sizeof(c->vel));
		c->speed = (car.have & SWOFTY_LIVE_HAVE_SPEED) ? car.speed
		                                               : sqrtf(car.world_vel[0] * car.world_vel[0] + car.world_vel[1] * car.world_vel[1] + car.world_vel[2] * car.world_vel[2]);
		if ((car.have & SWOFTY_LIVE_HAVE_TYRE) && car.tyre_letter) {
			c->known = 1;
			c->wet = car.tyre_letter == 'W' ? 2 : car.tyre_letter == 'I' ? 1 : 0;
			tyres++;
		}
		if ((int)v == subject) {
			c->player = 1;
			if (car.have & SWOFTY_LIVE_HAVE_STEER)
				out->steer = car.steer;
		}
	}

	if (out->count > 0 && !(rain_logged & 2)) {
		rain_logged |= 2;
		swofty->log("openxr_vr_settings: rain feed: %u cars with positions, %d with tyres, player car %d", out->count, tyres, subject);
	}
	return out->count > 0;
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
	tear_off_init(api);
	if (api->version < 3 || !api->setting || !api->watch_setting) {
		api->log("openxr_vr_settings: this Swofty core has no settings API; the defaults apply");
		starting = 0;
		return 1;
	}
	static const char* ids[] = { "visor", "visor_colour", "render_scale", "vr_graphics", "rain_spray", "tear_offs", "tear_off_key" };
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
	static const char* toggles[] = { "view_window", "visor_drops" };
	for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); i++) {
		SwoftySettingValue toggle = { 0 };
		if (api->setting(toggles[i], SWOFTY_SETTING_TOGGLE, &toggle)) {
			toggle.type = SWOFTY_SETTING_TOGGLE;
			on_change(toggles[i], &toggle, NULL);
		}
		if (!api->watch_setting(toggles[i], on_change, NULL))
			api->log("openxr_vr_settings: can't watch %s; changes apply from the next start", toggles[i]);
	}
	starting = 0;
	api->log("openxr_vr_settings: visor %s, render scale %.2f (0: opencomposite.ini), view window %s, tear-offs %s", visor, render_scale,
	    view_window ? "on" : "off", tear_off_mode() == 0 ? "off" : tear_off_mode() == 2 ? "button only" : "automatic and button");
	return 1;
}
