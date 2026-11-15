#include "AddonSettings.h"

#include "XrDriverPrivate.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <mutex>

namespace {

struct Plugin {
	unsigned int (*visor)(char* out, unsigned int size) = nullptr;
	float (*renderScale)() = nullptr;
	int (*viewWindow)() = nullptr;
	int (*rain)(void* out, unsigned int size) = nullptr;
};

// The plugin's exports, looked up again at most once a second until it is found (it normally loads before VR starts)
Plugin Find()
{
	static std::mutex mutex;
	static Plugin plugin;
	static bool found = false;
	static unsigned long long nextLook = 0;
	std::lock_guard<std::mutex> guard(mutex);
#ifdef _WIN32
	if (!found && GetTickCount64() >= nextLook) {
		nextLook = GetTickCount64() + 1000;
		// bitey_openxr_vr.dll: its own name, so Swofty never takes it for the old liclf.openxr-vr plugin (openxr_vr_settings.dll),
		// whose settings it would then read
		if (HMODULE module = GetModuleHandleW(L"bitey_openxr_vr.dll")) {
			plugin.visor = (unsigned int (*)(char*, unsigned int))GetProcAddress(module, "OpenXrVrVisor");
			plugin.renderScale = (float (*)())GetProcAddress(module, "OpenXrVrRenderScale");
			plugin.viewWindow = (int (*)())GetProcAddress(module, "OpenXrVrViewWindow");
			plugin.rain = (int (*)(void*, unsigned int))GetProcAddress(module, "OpenXrVrRain");
			found = true;
			OOVR_LOGF("Addon settings plugin found (visor %s, render scale %s, rain %s)", plugin.visor ? "yes" : "no", plugin.renderScale ? "yes" : "no", plugin.rain ? "yes" : "no");
		}
	}
#endif
	return plugin;
}

} // namespace

unsigned int AddonSettings::Visor(std::string& visor)
{
	Plugin plugin = Find();
	if (plugin.visor) {
		char buffer[64] = {};
		unsigned int generation = plugin.visor(buffer, sizeof(buffer));
		visor = buffer;
		return generation;
	}
	visor = "clear";
	return 0;
}

float AddonSettings::RenderScale()
{
	Plugin plugin = Find();
	return plugin.renderScale ? plugin.renderScale() : 0.0f;
}

bool AddonSettings::ViewWindow()
{
	Plugin plugin = Find();
	return plugin.viewWindow && plugin.viewWindow() != 0;
}

bool AddonSettings::RainFeed(Rain::Feed& feed)
{
	Plugin plugin = Find();
	return plugin.rain && plugin.rain(&feed, (unsigned int)sizeof(feed)) != 0;
}