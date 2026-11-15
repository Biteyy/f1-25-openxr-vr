#pragma once

#include <string>

#include "RainSim.h"

/**
 * The Swofty addon's settings, from its engine plugin (native/bitey_openxr_vr.dll, built from swofty-addon/openxr_vr_settings.c) when that is loaded: it holds what
 * the player picked in the launcher or in the game's menus. Without the plugin (eg the DLL used outside Swofty)
 * every setting has its default.
 */
namespace AddonSettings {

/**
 * The visor id ("off", "rgba_RRGGBBAA" for one colour, or an id whose image helmet_visor_<id>.png sits next to
 * openvr_api.dll), and a number that changes whenever it does. Without the plugin: "clear", number 0.
 */
unsigned int Visor(std::string& visor);

/** Render resolution scale per axis, or 0 for opencomposite.ini's supersampleRatio. */
float RenderScale();

/** Whether the VR view window (see MirrorWindow) is on. Without the plugin: off. */
bool ViewWindow();

/**
 * Fills feed with the cars around the player and the rain settings (tyre spray, visor drops), for RainSim.
 * False while there is no live session or no plugin: then nothing is drawn.
 */
bool RainFeed(Rain::Feed& feed);

/** How much the game's sound is muffled, as inside a helmet (see HelmetAudio): 0 off, 1 light, 2 strong. */
int HelmetSound();

} // namespace AddonSettings
