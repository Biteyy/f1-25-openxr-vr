/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Opens the addon's page from its tile in the game's Settings menu (category cat_openxr_vr).
 *
 * Pressing a Settings tile calls the settings home's category-select function with the category's name. For a
 * stock tile the game then opens it itself; for an added one it sends "open <ui_command>" to the settings container,
 * which nothing handles, so the menu froze. This hooks the category-select function: for cat_openxr_vr it raises the
 * event t_openxr_vr on the settings container (the addon's settings_container.ui routes that event to its page) and
 * returns before the stock code runs. A second hook in the container's command handler remembers the container.
 *
 * The same recipe as RK's AI Special Liveries (More Special Liveries, GPLv3), which hooks the entries of these two
 * functions. These hooks sit further in (past the bytes its signatures cover and its entry hooks rewrite), are
 * installed only after it has scanned, and leave every other category to it and to the game.
 *
 * Each hook is a 5-byte jump to a stub allocated within 2 GB of the game, which saves the volatile registers, calls
 * a C handler, and then runs the instructions it replaced. The bytes at both sites are checked first; on any
 * mismatch (a game update) nothing is patched and the tile simply does nothing.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <string.h>

#include "swofty_api.h"

#define CATEGORY "cat_openxr_vr"
#define EVENT "t_openxr_vr"

/* Function entries, by signature (the same as AI Special Liveries'): */
/* the settings home's category select: void(home *this, const char *category) */
static const char kSelectSig[] = "41 57 41 56 56 57 53 48 83 EC 30 48 85 D2 74 ?? 48 89 CE 48 83 B9 A0 08 00 00 00 74";
/* the settings container's command handler: (container *this, NeStr **command, data *) */
static const char kCommandSig[] = "56 57 53 48 81 EC 90 00 00 00 48 8B 02 48 85 C0 0F 84 ?? ?? ?? ?? 4C 89 C7 48 89 CE 8B 00 3D E9 84 40 1B";
/* the UI event raiser: (node *, const char *event, args *, flags) */
static const char kRaiseSig[] = "41 57 41 56 41 54 56 57 53 48 81 EC 98 00 00 00 4C 89 CF 4D 89 C6 48 89 CE 48 C7 44 24 28 00 00 00 00 48 C7 44 24 30 41";

/* Hook sites inside those functions, and the instructions there (moved into the stubs) */
#define SELECT_SITE 0x1d /* mov rdi, rdx; mov word [rsi+0x8b1], 0 — rdx is the category */
static const uint8_t kSelectBytes[] = { 0x48, 0x89, 0xD7, 0x66, 0xC7, 0x86, 0xB1, 0x08, 0x00, 0x00, 0x00, 0x00 };
#define SELECT_EXIT 0x75 /* the function's epilogue: add rsp, 0x30; pops; ret */
static const uint8_t kSelectExitBytes[] = { 0x48, 0x83, 0xC4, 0x30, 0x5B, 0x5F, 0x5E, 0x41, 0x5E, 0x41, 0x5F, 0xC3 };
#define COMMAND_SITE 0x16 /* mov rdi, r8; mov rsi, rcx — rcx is the container */
static const uint8_t kCommandBytes[] = { 0x4C, 0x89, 0xC7, 0x48, 0x89, 0xCE };

static const SwoftyApi* api;
static uintptr_t select_fn, command_fn, raise_fn;
static void* volatile container; /* the settings container that last ran a command */
static uint32_t frame_token;
static int frames;

typedef uint64_t (*RaiseFn)(void* node, const char* event, void* args, uint64_t flags);

/* Called by the stubs, on the game's UI thread */
static void on_command(void* this_container)
{
	container = this_container;
}

static int on_select(const char* category)
{
	if (!category || strcmp(category, CATEGORY) != 0)
		return 0;
	void* target = container;
	if (!target || !raise_fn) {
		api->log("openxr_vr_settings: tile pressed, but no settings container has run a command yet; left to the game");
		return 0;
	}
	uint64_t args[3] = { 4, 0, 0 }; /* as AI Special Liveries passes them */
	((RaiseFn)raise_fn)(target, EVENT, args, 0);
	api->log("openxr_vr_settings: tile pressed: raised " EVENT " on the settings container");
	return 1;
}

/* ---- Code generation ---------------------------------------------------------------------------------------------- */

typedef struct {
	uint8_t* p;
} Emit;

static void put(Emit* e, const void* bytes, size_t n)
{
	memcpy(e->p, bytes, n);
	e->p += n;
}

static void put_u64(Emit* e, uint64_t v)
{
	put(e, &v, 8);
}

static void put_jmp_abs(Emit* e, uintptr_t target) /* jmp [rip+0]; dq target */
{
	static const uint8_t op[] = { 0xFF, 0x25, 0, 0, 0, 0 };
	put(e, op, sizeof(op));
	put_u64(e, target);
}

static const uint8_t kSave[] = { 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, /* push rax..r11 */
	0x48, 0x83, 0xEC, 0x28 }; /* sub rsp, 0x28 (shadow space; the sites have rsp 16-aligned, the pushes add 8) */
static const uint8_t kRestoreRsp[] = { 0x48, 0x83, 0xC4, 0x28 }; /* add rsp, 0x28 */
static const uint8_t kPops[] = { 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 }; /* pop r11..rax */

static void put_call(Emit* e, void* fn) /* mov rax, imm64; call rax */
{
	static const uint8_t mov[] = { 0x48, 0xB8 };
	static const uint8_t call[] = { 0xFF, 0xD0 };
	put(e, mov, 2);
	put_u64(e, (uint64_t)(uintptr_t)fn);
	put(e, call, 2);
}

/* Executable memory within rel32 reach of the game's code: the first free region found walking out from it.
   NULL when every region in reach is taken (the game reserves much of the space around its image). */
static uint8_t* alloc_near(uintptr_t near_to, size_t size)
{
	const uintptr_t reach = 0x70000000;
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	uintptr_t gran = si.dwAllocationGranularity;
	uintptr_t lo = near_to > reach ? near_to - reach : (uintptr_t)si.lpMinimumApplicationAddress;
	uintptr_t hi = near_to + reach;
	MEMORY_BASIC_INFORMATION mbi;
	for (uintptr_t at = lo; at < hi && VirtualQuery((void*)at, &mbi, sizeof(mbi)); at = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
		if (mbi.State != MEM_FREE)
			continue;
		uintptr_t start = ((uintptr_t)mbi.BaseAddress + gran - 1) & ~(gran - 1);
		if (start + size > (uintptr_t)mbi.BaseAddress + mbi.RegionSize || start + size > hi || start < lo)
			continue;
		void* p = VirtualAlloc((void*)start, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (p)
			return p;
	}
	return NULL;
}

/* Replaces `replaced` bytes at site with a jump to stub: jmp rel32 when the stub is in reach, else
   jmp [rip+0]; dq stub (14 bytes); the rest with nops */
static int far_jumps; /* set by the offline test to try the 14-byte path */

static int write_jump(uintptr_t site, size_t replaced, const uint8_t* stub)
{
	uint8_t patch[32];
	intptr_t rel = (intptr_t)stub - (intptr_t)(site + 5);
	if (!far_jumps && rel > -0x7FFFFFF0LL && rel < 0x7FFFFFF0LL) {
		patch[0] = 0xE9;
		int32_t rel32 = (int32_t)rel;
		memcpy(patch + 1, &rel32, 4);
		for (size_t i = 5; i < replaced; i++)
			patch[i] = 0x90;
	} else {
		if (replaced < 14)
			return 0;
		static const uint8_t op[] = { 0xFF, 0x25, 0, 0, 0, 0 };
		memcpy(patch, op, 6);
		uint64_t target = (uint64_t)(uintptr_t)stub;
		memcpy(patch + 6, &target, 8);
		for (size_t i = 14; i < replaced; i++)
			patch[i] = 0x90;
	}
	DWORD old;
	if (!VirtualProtect((void*)site, replaced, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	memcpy((void*)site, patch, replaced);
	VirtualProtect((void*)site, replaced, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)site, replaced);
	return 1;
}

/* With a 14-byte jump (no memory in rel32 reach) more instructions move into the stubs:
   category select +0x1d..+0x30 adds lea rbx, [rsi+0x670]; the command handler +0x16..+0x29 adds
   mov eax, [rax]; cmp eax, 0x1b4084e9; jle rel32 (the jle is re-made with an absolute target) */
static const uint8_t kSelectMoreBytes[] = { 0x48, 0x8D, 0x9E, 0x70, 0x06, 0x00, 0x00 };
static const uint8_t kCommandMoreBytes[] = { 0x8B, 0x00, 0x3D, 0xE9, 0x84, 0x40, 0x1B };
static const uint8_t kCommandJle[] = { 0x0F, 0x8E }; /* followed by rel32 */

static void install(void)
{
	uintptr_t select_site = select_fn + SELECT_SITE, command_site = command_fn + COMMAND_SITE;
	if (memcmp((void*)select_site, kSelectBytes, sizeof(kSelectBytes)) != 0
	    || memcmp((void*)(select_fn + SELECT_EXIT), kSelectExitBytes, sizeof(kSelectExitBytes)) != 0
	    || memcmp((void*)command_site, kCommandBytes, sizeof(kCommandBytes)) != 0) {
		api->log("openxr_vr_settings: the Settings menu code isn't as expected (game update?); the VR tile does nothing");
		return;
	}
	uint8_t* mem = far_jumps ? NULL : alloc_near(select_fn, 4096);
	int near_jumps = mem != NULL;
	if (!mem) {
		/* Long jumps: the extra instructions they cover must be the expected ones too */
		uintptr_t more_select = select_site + sizeof(kSelectBytes), more_command = command_site + sizeof(kCommandBytes);
		if (memcmp((void*)more_select, kSelectMoreBytes, sizeof(kSelectMoreBytes)) != 0
		    || memcmp((void*)more_command, kCommandMoreBytes, sizeof(kCommandMoreBytes)) != 0
		    || memcmp((void*)(more_command + sizeof(kCommandMoreBytes)), kCommandJle, sizeof(kCommandJle)) != 0) {
			api->log("openxr_vr_settings: the Settings menu code isn't as expected for a long jump; the VR tile does nothing");
			return;
		}
		mem = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (!mem) {
			api->log("openxr_vr_settings: no executable memory; the VR tile does nothing");
			return;
		}
	}
	size_t select_len = sizeof(kSelectBytes) + (near_jumps ? 0 : sizeof(kSelectMoreBytes));
	size_t command_len = sizeof(kCommandBytes) + (near_jumps ? 0 : sizeof(kCommandMoreBytes) + 6);

	/* Command handler stub: remember rcx (the container), then the moved instructions, then back */
	uint8_t* command_stub = mem;
	Emit e = { command_stub };
	put(&e, kSave, sizeof(kSave));
	put_call(&e, (void*)on_command); /* rcx is already the container */
	put(&e, kRestoreRsp, sizeof(kRestoreRsp));
	put(&e, kPops, sizeof(kPops));
	put(&e, kCommandBytes, sizeof(kCommandBytes));
	if (!near_jumps) {
		put(&e, kCommandMoreBytes, sizeof(kCommandMoreBytes));
		uintptr_t jle_at = command_site + sizeof(kCommandBytes) + sizeof(kCommandMoreBytes);
		int32_t rel;
		memcpy(&rel, (void*)(jle_at + 2), 4);
		static const uint8_t jg_over[] = { 0x7F, 14 }; /* jg: not taken by jle, skip its jump */
		put(&e, jg_over, sizeof(jg_over));
		put_jmp_abs(&e, jle_at + 6 + rel);
	}
	put_jmp_abs(&e, command_site + command_len);

	/* Category select stub: on_select(rdx); non-zero leaves through the epilogue, else the moved instructions */
	uint8_t* select_stub = (uint8_t*)(((uintptr_t)e.p + 15) & ~(uintptr_t)15);
	e.p = select_stub;
	static const uint8_t mov_rcx_rdx[] = { 0x48, 0x89, 0xD1 };
	static const uint8_t test_eax[] = { 0x85, 0xC0 };
	put(&e, kSave, sizeof(kSave));
	put(&e, mov_rcx_rdx, sizeof(mov_rcx_rdx));
	put_call(&e, (void*)on_select);
	put(&e, kRestoreRsp, sizeof(kRestoreRsp));
	put(&e, test_eax, sizeof(test_eax)); /* the pops leave the flags alone */
	put(&e, kPops, sizeof(kPops));
	static const uint8_t jnz[] = { 0x75, 0x00 }; /* jnz skip (rel8, patched below) */
	uint8_t* jnz_at = e.p;
	put(&e, jnz, sizeof(jnz));
	put(&e, kSelectBytes, sizeof(kSelectBytes));
	if (!near_jumps)
		put(&e, kSelectMoreBytes, sizeof(kSelectMoreBytes));
	put_jmp_abs(&e, select_site + select_len);
	jnz_at[1] = (uint8_t)(e.p - (jnz_at + 2));
	put_jmp_abs(&e, select_fn + SELECT_EXIT);
	FlushInstructionCache(GetCurrentProcess(), mem, 4096);

	if (!write_jump(command_site, command_len, command_stub) || !write_jump(select_site, select_len, select_stub)) {
		api->log("openxr_vr_settings: couldn't patch the Settings menu code; the VR tile does nothing");
		return;
	}
	api->log("openxr_vr_settings: VR tile hooks in (%s jumps; category select +0x%x, container command +0x%x)",
	    near_jumps ? "near" : "long", SELECT_SITE, COMMAND_SITE);
}

/* After the window-stage plugins (AI Special Liveries scans these functions' entries then) */
static void on_frame(double dt, void* user)
{
	(void)dt;
	(void)user;
	if (++frames == 60)
		install(); /* once; the callback stays subscribed but does nothing after this */
}

/* Called from SwoftyAddonInit: finds the functions now (before other plugins hook their entries) and installs the
   hooks a little later */
void settings_tile_init(const SwoftyApi* swofty)
{
	api = swofty;
	if (!api->scan) {
		api->log("openxr_vr_settings: no scan in this Swofty core; the VR tile does nothing");
		return;
	}
	int a = api->scan(".xtls", kSelectSig, &select_fn);
	int b = api->scan(".xtls", kCommandSig, &command_fn);
	int c = api->scan(".xtls", kRaiseSig, &raise_fn);
	if (a != 1 || b != 1 || c != 1) {
		api->log("openxr_vr_settings: Settings menu code found %d/%d/%d time(s), want 1 each; the VR tile does nothing", a, b, c);
		select_fn = command_fn = raise_fn = 0;
		return;
	}
	/* The events table uses the SWOFTY_ASSET_* result codes: 0 is ok */
	if (!api->events || !api->events->on_frame || api->events->on_frame(on_frame, NULL, &frame_token) != SWOFTY_ASSET_OK) {
		api->log("openxr_vr_settings: no frame events; installing the VR tile hooks now");
		install();
	}
}
