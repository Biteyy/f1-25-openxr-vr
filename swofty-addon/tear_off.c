/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The tear-off button: any key, wheel or joystick button (or hat direction), or controller button, picked in the
 * addon's settings by pressing it. Swofty settings have no key binding type, so the choice "tear_off_key" has a
 * "learn" value: switching to it while the game runs takes the next press anywhere (within 10 s) as the button,
 * saves it in the addon's storage and says which it is on screen. Each press of the saved button counts up, and
 * openvr_api.dll (DrvOpenXR/RainSim) pulls a tear-off on every new count.
 *
 * Input is polled on a thread of its own every 10 ms: the keyboard (only while the game is in front, so typing
 * elsewhere does nothing), wheels and joysticks through winmm (which sees DirectInput devices; up to 32 buttons and
 * the hat), and Xbox-style controllers through XInput. Both DLLs are part of Windows and loaded when needed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "swofty_api.h"

#define BIND_FILE "tearoff_bind.txt"
#define MAX_JOYS 16

enum { BIND_NONE = 0, BIND_KEY = 1, BIND_JOY = 2, BIND_PAD = 3 };

/* What a press is: a key (code = virtual key), a joystick input (device = its USB vendor and product, code = button
   0..31, or 32..35 for the hat up, right, down, left), or a controller input (code = XInput button bit, or 16/17 for
   the left/right trigger) */
typedef struct {
	int kind;
	uint32_t device;
	uint32_t code;
} Input;

static const SwoftyApi* api;
static SRWLOCK lock = SRWLOCK_INIT;
static Input bind;
static volatile LONG pulls;
static volatile LONG mode = 1; /* 0 off, 1 automatic and the button, 2 the button only */
static volatile LONG learning; /* set by the setting; the thread takes the next press */
static ULONGLONG learn_until;
static char message[256]; /* waiting to be shown on the game's thread */
static char message_detail[256];
static uint32_t frame_token;

/* ---- winmm and XInput, loaded when first needed ---- */

typedef MMRESULT(WINAPI* JoyGetPosExFn)(UINT, LPJOYINFOEX);
typedef MMRESULT(WINAPI* JoyGetDevCapsFn)(UINT_PTR, LPJOYCAPSA, UINT);
typedef struct {
	WORD wButtons;
	BYTE bLeftTrigger, bRightTrigger;
	SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} PadState;
typedef struct {
	DWORD dwPacketNumber;
	PadState Gamepad;
} PadInfo;
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, PadInfo*);

static JoyGetPosExFn joy_pos;
static JoyGetDevCapsFn joy_caps;
static XInputGetStateFn pad_state;

static void load_input_dlls(void)
{
	HMODULE winmm = LoadLibraryW(L"winmm.dll");
	if (winmm) {
		joy_pos = (JoyGetPosExFn)GetProcAddress(winmm, "joyGetPosEx");
		joy_caps = (JoyGetDevCapsFn)GetProcAddress(winmm, "joyGetDevCapsA");
	}
	HMODULE xinput = LoadLibraryW(L"xinput1_4.dll");
	if (!xinput)
		xinput = LoadLibraryW(L"xinput9_1_0.dll");
	if (xinput)
		pad_state = (XInputGetStateFn)GetProcAddress(xinput, "XInputGetState");
}

/* ---- Names, for the message ---- */

static void device_name(uint32_t device, char* out, size_t cap)
{
	/* Windows keeps the game controller's own name under its vendor and product ids */
	char key[160], name[128];
	DWORD size = sizeof(name);
	snprintf(key, sizeof(key), "System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_%04X&PID_%04X",
	    (unsigned)(device >> 16), (unsigned)(device & 0xffff));
	if (RegGetValueA(HKEY_CURRENT_USER, key, "OEMName", RRF_RT_REG_SZ, NULL, name, &size) == ERROR_SUCCESS
	    || (size = sizeof(name), RegGetValueA(HKEY_LOCAL_MACHINE, key, "OEMName", RRF_RT_REG_SZ, NULL, name, &size) == ERROR_SUCCESS))
		snprintf(out, cap, "%s", name);
	else
		snprintf(out, cap, "Wheel/joystick %04X:%04X", (unsigned)(device >> 16), (unsigned)(device & 0xffff));
}

static void input_name(const Input* in, char* out, size_t cap)
{
	if (in->kind == BIND_KEY) {
		char name[64] = "";
		UINT scan = MapVirtualKeyA(in->code, MAPVK_VK_TO_VSC);
		LONG lparam = (LONG)(scan << 16);
		if (in->code == VK_RCONTROL || in->code == VK_RMENU || in->code == VK_INSERT || in->code == VK_DELETE || in->code == VK_HOME
		    || in->code == VK_END || in->code == VK_PRIOR || in->code == VK_NEXT || (in->code >= VK_LEFT && in->code <= VK_DOWN))
			lparam |= 1 << 24; /* extended keys */
		if (!scan || !GetKeyNameTextA(lparam, name, sizeof(name)))
			snprintf(name, sizeof(name), "key 0x%02X", (unsigned)in->code);
		snprintf(out, cap, "Keyboard: %s", name);
	} else if (in->kind == BIND_JOY) {
		static const char* hats[] = { "hat up", "hat right", "hat down", "hat left" };
		char dev[128], what[32];
		device_name(in->device, dev, sizeof(dev));
		if (in->code < 32)
			snprintf(what, sizeof(what), "button %u", (unsigned)in->code + 1);
		else
			snprintf(what, sizeof(what), "%s", hats[(in->code - 32) & 3]);
		snprintf(out, cap, "%s %s", dev, what);
	} else if (in->kind == BIND_PAD) {
		static const struct {
			uint32_t bit;
			const char* name;
		} names[] = { { 0x0001, "D-pad up" }, { 0x0002, "D-pad down" }, { 0x0004, "D-pad left" }, { 0x0008, "D-pad right" },
			{ 0x0010, "Start" }, { 0x0020, "Back" }, { 0x0040, "left stick press" }, { 0x0080, "right stick press" },
			{ 0x0100, "LB" }, { 0x0200, "RB" }, { 0x1000, "A" }, { 0x2000, "B" }, { 0x4000, "X" }, { 0x8000, "Y" },
			{ 1u << 16, "LT" }, { 1u << 17, "RT" } };
		const char* n = "button";
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (names[i].bit == in->code)
				n = names[i].name;
		snprintf(out, cap, "Controller %s", n);
	} else {
		snprintf(out, cap, "none");
	}
}

/* ---- Storage ---- */

static void save_bind(const Input* in)
{
	char text[96];
	int n = snprintf(text, sizeof(text), "%d %u %u\n", in->kind, (unsigned)in->device, (unsigned)in->code);
	if (api->storage && api->storage->available && api->storage->available() && api->storage->write)
		api->storage->write(BIND_FILE, text, (size_t)n);
	else
		api->log("openxr_vr_settings: no addon storage, so the tear-off button lasts until the game closes");
}

static void load_bind(void)
{
	char text[96] = "";
	size_t len = 0;
	if (!api->storage || !api->storage->available || !api->storage->available() || !api->storage->read
	    || api->storage->read(BIND_FILE, text, sizeof(text) - 1, &len) != 0)
		return;
	text[len < sizeof(text) ? len : sizeof(text) - 1] = 0;
	Input in = { 0 };
	unsigned device = 0, code = 0;
	if (sscanf_s(text, "%d %u %u", &in.kind, &device, &code) == 3 && in.kind >= BIND_KEY && in.kind <= BIND_PAD) {
		in.device = device;
		in.code = code;
		bind = in;
		char name[192];
		input_name(&bind, name, sizeof(name));
		api->log("openxr_vr_settings: tear-off button: %s", name);
	}
}

/* ---- Polling ---- */

typedef struct {
	BYTE keys[256];
	uint32_t joy_buttons[MAX_JOYS]; /* buttons 0..31 */
	uint32_t joy_hat[MAX_JOYS]; /* bits 0..3: up, right, down, left */
	uint32_t joy_device[MAX_JOYS]; /* vendor << 16 | product, 0: not there */
	uint32_t pad[4];
} State;

static int game_in_front(void)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

static void poll(State* s, ULONGLONG now, ULONGLONG* next_probe)
{
	memset(s->keys, 0, sizeof(s->keys));
	if (game_in_front()) {
		for (int vk = 0x08; vk < 0xFF; vk++) {
			if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
				continue; /* the left/right versions are read instead */
			s->keys[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
		}
	}
	/* Joysticks: every slot now and then (a missing one is slow to ask), else only those that answered */
	int probe = now >= *next_probe;
	if (probe)
		*next_probe = now + 3000;
	for (UINT id = 0; id < MAX_JOYS && joy_pos; id++) {
		if (!probe && !s->joy_device[id])
			continue;
		JOYINFOEX info;
		memset(&info, 0, sizeof(info));
		info.dwSize = sizeof(info);
		info.dwFlags = JOY_RETURNBUTTONS | JOY_RETURNPOV;
		if (joy_pos(id, &info) != JOYERR_NOERROR) {
			s->joy_device[id] = 0;
			s->joy_buttons[id] = s->joy_hat[id] = 0;
			continue;
		}
		if (!s->joy_device[id]) {
			JOYCAPSA caps;
			memset(&caps, 0, sizeof(caps));
			s->joy_device[id] = joy_caps && joy_caps(id, &caps, sizeof(caps)) == JOYERR_NOERROR ? ((uint32_t)caps.wMid << 16 | caps.wPid) : 1;
		}
		s->joy_buttons[id] = info.dwButtons;
		uint32_t hat = 0;
		if (info.dwPOV != JOY_POVCENTERED && info.dwPOV <= 36000) {
			DWORD a = info.dwPOV;
			if (a >= 31500 || a <= 4500) hat |= 1;
			if (a >= 4500 && a <= 13500) hat |= 2;
			if (a >= 13500 && a <= 22500) hat |= 4;
			if (a >= 22500 && a <= 31500) hat |= 8;
		}
		s->joy_hat[id] = hat;
	}
	for (DWORD i = 0; i < 4 && pad_state; i++) {
		PadInfo info;
		memset(&info, 0, sizeof(info));
		if (pad_state(i, &info) != ERROR_SUCCESS) {
			s->pad[i] = 0;
			continue;
		}
		s->pad[i] = info.Gamepad.wButtons | (info.Gamepad.bLeftTrigger > 200 ? 1u << 16 : 0) | (info.Gamepad.bRightTrigger > 200 ? 1u << 17 : 0);
	}
}

/* The first input down now that wasn't before; 0 if none */
static int fresh_press(const State* now, const State* before, Input* out)
{
	for (int vk = 0; vk < 256; vk++)
		if (now->keys[vk] && !before->keys[vk]) {
			out->kind = BIND_KEY, out->device = 0, out->code = (uint32_t)vk;
			return 1;
		}
	for (int i = 0; i < 4; i++) {
		uint32_t down = now->pad[i] & ~before->pad[i];
		for (uint32_t b = 0; b < 18; b++)
			if (down & (1u << b)) {
				out->kind = BIND_PAD, out->device = 0, out->code = 1u << b;
				return 1;
			}
	}
	for (int id = 0; id < MAX_JOYS; id++) {
		if (!now->joy_device[id] || now->joy_device[id] != before->joy_device[id])
			continue;
		uint32_t down = now->joy_buttons[id] & ~before->joy_buttons[id];
		for (uint32_t b = 0; b < 32; b++)
			if (down & (1u << b)) {
				out->kind = BIND_JOY, out->device = now->joy_device[id], out->code = b;
				return 1;
			}
		uint32_t hat = now->joy_hat[id] & ~before->joy_hat[id];
		for (uint32_t b = 0; b < 4; b++)
			if (hat & (1u << b)) {
				out->kind = BIND_JOY, out->device = now->joy_device[id], out->code = 32 + b;
				return 1;
			}
	}
	return 0;
}

/* Whether the bound input went down now */
static int bound_pressed(const State* now, const State* before, const Input* b)
{
	if (b->kind == BIND_KEY)
		return b->code < 256 && now->keys[b->code] && !before->keys[b->code];
	if (b->kind == BIND_PAD) {
		for (int i = 0; i < 4; i++)
			if ((now->pad[i] & b->code) && !(before->pad[i] & b->code))
				return 1;
		return 0;
	}
	if (b->kind == BIND_JOY) {
		/* The device it was learnt on; the joystick slot may differ from one start to the next */
		for (int id = 0; id < MAX_JOYS; id++) {
			if (now->joy_device[id] != b->device || before->joy_device[id] != b->device)
				continue;
			if (b->code < 32 ? ((now->joy_buttons[id] >> b->code) & 1) && !((before->joy_buttons[id] >> b->code) & 1)
			                 : ((now->joy_hat[id] >> (b->code - 32)) & 1) && !((before->joy_hat[id] >> (b->code - 32)) & 1))
				return 1;
		}
	}
	return 0;
}

static void say(const char* text, const char* detail)
{
	AcquireSRWLockExclusive(&lock);
	snprintf(message, sizeof(message), "%s", text);
	snprintf(message_detail, sizeof(message_detail), "%s", detail ? detail : "");
	ReleaseSRWLockExclusive(&lock);
}

static DWORD WINAPI input_thread(void* unused)
{
	(void)unused;
	load_input_dlls();
	static State a, b;
	State *now = &a, *before = &b;
	ULONGLONG next_probe = 0;
	poll(before, GetTickCount64(), &next_probe);
	for (;;) {
		Sleep(10);
		ULONGLONG t = GetTickCount64();
		poll(now, t, &next_probe);
		Input pressed;
		if (learning) {
			if (fresh_press(now, before, &pressed)) {
				InterlockedExchange(&learning, 0);
				AcquireSRWLockExclusive(&lock);
				bind = pressed;
				ReleaseSRWLockExclusive(&lock);
				save_bind(&pressed);
				char name[192], text[256];
				input_name(&pressed, name, sizeof(name));
				api->log("openxr_vr_settings: tear-off button learnt: %s", name);
				snprintf(text, sizeof(text), "Tear-off button: %s", name);
				say(text, "Set \"Tear-off button\" back to \"Use the saved button\".");
			} else if (t > learn_until) {
				InterlockedExchange(&learning, 0);
				say("No button pressed: the tear-off button stays as it was", NULL);
			}
		} else if (mode != 0) {
			AcquireSRWLockShared(&lock);
			Input b2 = bind;
			ReleaseSRWLockShared(&lock);
			if (b2.kind != BIND_NONE && bound_pressed(now, before, &b2))
				InterlockedIncrement(&pulls);
		}
		State* swap = now;
		now = before;
		before = swap;
	}
}

/* ---- The game's thread: show waiting messages ---- */

typedef const SwoftyHud* (*SwoftyCoreHudFn)(void);

static void on_frame(double dt, void* user)
{
	(void)dt;
	(void)user;
	char text[256], detail[256];
	AcquireSRWLockExclusive(&lock);
	strcpy_s(text, sizeof(text), message);
	strcpy_s(detail, sizeof(detail), message_detail);
	message[0] = message_detail[0] = 0;
	ReleaseSRWLockExclusive(&lock);
	if (!text[0])
		return;
	/* In a session the race HUD's notice; in the menus a popup */
	HMODULE core = GetModuleHandleW(L"swofty_core.dll");
	SwoftyCoreHudFn get_hud = core ? (SwoftyCoreHudFn)GetProcAddress(core, "SwoftyCoreHud") : NULL;
	const SwoftyHud* hud = get_hud ? get_hud() : NULL;
	if (hud && hud->available && hud->available() && hud->toast && hud->toast(text, detail, 5.0f, NULL) == 0)
		return;
	if (api->popup && api->popup->available && api->popup->available() && api->popup->show) {
		char body[520];
		snprintf(body, sizeof(body), detail[0] ? "%s\n%s" : "%s%s", text, detail);
		api->popup->show("Tear-off button", body);
	}
}

/* ---- The settings ---- */

void tear_off_setting(const char* id, const char* choice, int at_start)
{
	if (!id || !choice)
		return;
	if (strcmp(id, "tear_offs") == 0) {
		LONG m = strcmp(choice, "off") == 0 ? 0 : strcmp(choice, "button") == 0 ? 2 : 1;
		InterlockedExchange(&mode, m);
	} else if (strcmp(id, "tear_off_key") == 0) {
		/* Only a switch to "learn" while the game runs learns: at start it would take the first press of a race */
		if (strcmp(choice, "learn") == 0 && !at_start) {
			learn_until = GetTickCount64() + 10000;
			InterlockedExchange(&learning, 1);
			say("Press the key or button for tear-offs", "On the keyboard, a wheel, a joystick or a controller, within 10 seconds.");
		} else {
			InterlockedExchange(&learning, 0);
		}
	}
}

uint32_t tear_off_mode(void)
{
	return (uint32_t)mode;
}

uint32_t tear_off_pulls(void)
{
	return (uint32_t)pulls;
}

void tear_off_init(const SwoftyApi* swofty)
{
	api = swofty;
	load_bind();
	if (api->events && api->events->on_frame)
		api->events->on_frame(on_frame, NULL, &frame_token);
	HANDLE thread = CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
	if (thread)
		CloseHandle(thread);
	else
		api->log("openxr_vr_settings: couldn't start the tear-off button's input thread");
}
