#pragma once

/**
 * Helmet sound: the game's audio muffled as heard inside a helmet, the Swofty addon's "helmet_sound" setting (off,
 * light, strong). openvr_api.dll runs inside the game, so it catches the game's sound on its way to Windows: the
 * WASAPI render client's GetBuffer/ReleaseBuffer (shared by every render client in the process) are patched, and each
 * buffer the game fills runs through a low-pass filter just before it's handed back. Only 32-bit float shared-mode
 * streams (Windows' usual mix format) are touched; anything else plays as it is.
 *
 * Windows only; Install is a no-op elsewhere.
 */
namespace HelmetAudio {

/** Patches the render client once (safe to call every frame). Logs whether it took. */
void Install();

} // namespace HelmetAudio
