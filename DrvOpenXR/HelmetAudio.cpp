#include "HelmetAudio.h"

#ifdef _WIN32

#include "AddonSettings.h"
#include "XrDriverPrivate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
#include <cmath>
#include <mutex>

namespace {

// IAudioRenderClient's vtable slots: IUnknown's three, then GetBuffer, ReleaseBuffer
const int kGetBuffer = 3, kReleaseBuffer = 4;

typedef HRESULT(STDMETHODCALLTYPE* GetBufferFn)(IAudioRenderClient*, UINT32, BYTE**);
typedef HRESULT(STDMETHODCALLTYPE* ReleaseBufferFn)(IAudioRenderClient*, UINT32, DWORD);
GetBufferFn realGetBuffer = nullptr;
ReleaseBufferFn realReleaseBuffer = nullptr;

// The mix format the filter assumes (the game's shared-mode streams use it)
unsigned channels = 0, sampleRate = 0;
bool floatSamples = false;

// Each render client's buffer between GetBuffer and ReleaseBuffer, and its filter state
const int kClients = 8, kMaxChannels = 8;
struct Client {
	IAudioRenderClient* client = nullptr;
	BYTE* buffer = nullptr;
	float low1[kMaxChannels] = {}, low2[kMaxChannels] = {};
};
Client clients[kClients];
std::mutex clientsMutex;

Client* Slot(IAudioRenderClient* client)
{
	for (Client& c : clients)
		if (c.client == client)
			return &c;
	for (Client& c : clients)
		if (!c.client) {
			c.client = client;
			return &c;
		}
	return nullptr;
}

HRESULT STDMETHODCALLTYPE HookedGetBuffer(IAudioRenderClient* self, UINT32 frames, BYTE** data)
{
	HRESULT hr = realGetBuffer(self, frames, data);
	if (SUCCEEDED(hr) && data) {
		std::lock_guard<std::mutex> guard(clientsMutex);
		if (Client* c = Slot(self))
			c->buffer = *data;
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedReleaseBuffer(IAudioRenderClient* self, UINT32 frames, DWORD flags)
{
	int level = floatSamples && frames && !(flags & AUDCLNT_BUFFERFLAGS_SILENT) ? AddonSettings::HelmetSound() : 0;
	if (level > 0) {
		std::lock_guard<std::mutex> guard(clientsMutex);
		Client* c = Slot(self);
		if (c && c->buffer) {
			float* s = (float*)c->buffer;
			unsigned ch = channels < (unsigned)kMaxChannels ? channels : (unsigned)kMaxChannels;
			if (level == 3) {
				// Open: the highs lifted about 6 dB above ~1.8 kHz (the signal plus what a low-pass leaves out), to undo
				// most of the muffle the game puts on its cockpit cameras; softly limited so loud moments don't clip
				float a = 1.0f - std::exp(-2.0f * 3.14159265f * 1800.0f / (float)sampleRate);
				for (UINT32 f = 0; f < frames; f++) {
					for (unsigned k = 0; k < ch; k++) {
						float& x = s[f * channels + k];
						c->low1[k] += a * (x - c->low1[k]);
						float y = x + 1.0f * (x - c->low1[k]);
						x = y / (1.0f + 0.25f * std::fabs(y));
					}
				}
			} else {
				// Two one-pole low-passes: highs fall away as through a padded helmet, the rumble stays.
				// Light: about 3 kHz; strong: about 1.4 kHz, a little quieter.
				float cutoff = level >= 2 ? 1400.0f : 3000.0f;
				float gain = level >= 2 ? 0.85f : 0.95f;
				float a = 1.0f - std::exp(-2.0f * 3.14159265f * cutoff / (float)sampleRate);
				for (UINT32 f = 0; f < frames; f++) {
					for (unsigned k = 0; k < ch; k++) {
						float& x = s[f * channels + k];
						c->low1[k] += a * (x - c->low1[k]);
						c->low2[k] += a * (c->low1[k] - c->low2[k]);
						x = c->low2[k] * gain;
					}
				}
			}
		}
	}
	{
		std::lock_guard<std::mutex> guard(clientsMutex);
		if (Client* c = Slot(self))
			c->buffer = nullptr;
	}
	return realReleaseBuffer(self, frames, flags);
}

bool Patch(void** vtable, int slot, void* hook, void** original)
{
	DWORD old;
	if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old))
		return false;
	*original = vtable[slot];
	vtable[slot] = hook;
	VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
	return true;
}

void DoInstall()
{
	HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	IMMDeviceEnumerator* enumerator = nullptr;
	IMMDevice* device = nullptr;
	IAudioClient* audio = nullptr;
	IAudioRenderClient* render = nullptr;
	WAVEFORMATEX* mix = nullptr;
	const char* failed = nullptr;

	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator)))
		failed = "no audio device enumerator";
	else if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
		failed = "no default playback device";
	else if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audio)))
		failed = "can't open the playback device";
	else if (FAILED(audio->GetMixFormat(&mix)))
		failed = "no mix format";
	else if (FAILED(audio->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr)))
		failed = "can't open a shared stream";
	else if (FAILED(audio->GetService(__uuidof(IAudioRenderClient), (void**)&render)))
		failed = "no render client";

	if (!failed) {
		channels = mix->nChannels;
		sampleRate = mix->nSamplesPerSec;
		floatSamples = mix->wBitsPerSample == 32
		    && (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
		        || (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
		// The vtable lives in audioses.dll and is shared by every render client, the game's included
		void** vtable = *(void***)render;
		if (!floatSamples)
			failed = "the mix format isn't 32-bit float";
		else if (!Patch(vtable, kGetBuffer, (void*)&HookedGetBuffer, (void**)&realGetBuffer)
		    || !Patch(vtable, kReleaseBuffer, (void*)&HookedReleaseBuffer, (void**)&realReleaseBuffer))
			failed = "can't patch the render client";
	}

	if (failed)
		OOVR_LOGF("Helmet sound: %s, so it is off", failed);
	else
		OOVR_LOGF("Helmet sound: ready (%u channels, %u Hz)", channels, sampleRate);

	if (render)
		render->Release();
	if (mix)
		CoTaskMemFree(mix);
	if (audio)
		audio->Release();
	if (device)
		device->Release();
	if (enumerator)
		enumerator->Release();
	if (SUCCEEDED(init))
		CoUninitialize();
}

} // namespace

void HelmetAudio::Install()
{
	static std::once_flag once;
	std::call_once(once, DoInstall);
}

#else

void HelmetAudio::Install() {}

#endif
