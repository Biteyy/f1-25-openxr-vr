#include "HelmetView.h"

#include "AddonSettings.h"

#include "../OpenOVR/Misc/lodepng.h"

#include <cmath>
#include <cstdio>
#include <vector>

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#endif

// The image spans +-this angle from straight ahead, across and up/down, on a flat plane (see make_helmet_view.py):
// well past any headset's field of view, so the tint covers everything the game shows.
static const float kHelmetHalfAngleDeg = 80.0f;

// How far in front of the eyes the plane sits, in metres: as close as a visor
static const float kHelmetDistance = 0.15f;

HelmetView::~HelmetView()
{
	Reset();
}

void HelmetView::Reset()
{
	if (chain != XR_NULL_HANDLE) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}
	chainWidth = chainHeight = 0;
	shownGeneration = ~0u; // reload the visor into the next session's swapchain
	showing = false;
	unsupported = false;
}

const XrCompositionLayerBaseHeader* HelmetView::GetLayer(const void* graphicsBinding)
{
	if (unsupported)
		return nullptr;

	std::string visor;
	unsigned int generation = AddonSettings::Visor(visor);
	if (generation != shownGeneration) {
		shownGeneration = generation;
		showing = visor != "off" && Show(graphicsBinding, visor);
	}
	if (!showing)
		return nullptr;

	// The view space belongs to the session's globals, which are recreated with each session
	layer.space = xr_gbl->viewSpace;
	return (const XrCompositionLayerBaseHeader*)&layer;
}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

static bool ReadVisorImage(const std::string& visor, std::vector<unsigned char>& rgba, unsigned& width, unsigned& height)
{
	// The id comes from the addon's settings; allow only what its values use, so it can't name another path
	if (visor.empty() || visor.size() > 40 || visor.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
		OOVR_LOGF("Helmet view: ignoring visor id '%s'", visor.c_str());
		return false;
	}

	// "rgba_RRGGBBAA": one colour, no image (the plugin's tint for the player's driver's visor)
	if (visor.size() == 13 && visor.compare(0, 5, "rgba_") == 0 && visor.find_first_not_of("0123456789abcdef", 5) == std::string::npos) {
		unsigned long packed = strtoul(visor.c_str() + 5, nullptr, 16);
		const unsigned char pixel[4] = { (unsigned char)(packed >> 24), (unsigned char)(packed >> 16), (unsigned char)(packed >> 8), (unsigned char)packed };
		width = height = 16;
		rgba.resize(width * height * 4);
		for (size_t i = 0; i < rgba.size(); i += 4)
			memcpy(&rgba[i], pixel, 4);
		return true;
	}

	wchar_t buffer[MAX_PATH];
	DWORD len = GetModuleFileNameW((HINSTANCE)&__ImageBase, buffer, MAX_PATH);
	std::wstring path(buffer, len);
	path = path.substr(0, path.rfind(L'\\') + 1) + L"helmet_visor_" + std::wstring(visor.begin(), visor.end()) + L".png";

	FILE* file = _wfopen(path.c_str(), L"rb");
	if (!file) {
		OOVR_LOGF("Helmet view: no helmet_visor_%s.png next to openvr_api.dll, so no visor", visor.c_str());
		return false;
	}
	std::vector<unsigned char> png;
	unsigned char chunk[65536];
	size_t got;
	while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0)
		png.insert(png.end(), chunk, chunk + got);
	fclose(file);

	unsigned err = lodepng::decode(rgba, width, height, png);
	if (err) {
		OOVR_LOGF("Helmet view: can't read helmet_visor_%s.png: %s", visor.c_str(), lodepng_error_text(err));
		return false;
	}
	return true;
}

bool HelmetView::Show(const void* graphicsBinding, const std::string& visor)
{
	const auto* binding = (const XrGraphicsBindingD3D12KHR*)graphicsBinding;
	if (!binding || binding->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
		OOVR_LOG("Helmet view: only Direct3D 12 games are supported, so it is off");
		unsupported = true;
		return false;
	}

	std::vector<unsigned char> rgba;
	unsigned width = 0, height = 0;
	if (!ReadVisorImage(visor, rgba, width, height))
		return false;

	auto fail = [](const char* what, long code) {
		OOVR_LOGF("Helmet view: %s failed (%ld), so no visor", what, code);
		return false;
	};

	// An sRGB RGBA or BGRA format: the PNG's colours are sRGB
	uint32_t formatCount = 0;
	XrResult res;
	if (XR_FAILED(res = xrEnumerateSwapchainFormats(xr_session.get(), 0, &formatCount, nullptr)))
		return fail("xrEnumerateSwapchainFormats", res);
	std::vector<int64_t> formats(formatCount);
	if (XR_FAILED(res = xrEnumerateSwapchainFormats(xr_session.get(), formatCount, &formatCount, formats.data())))
		return fail("xrEnumerateSwapchainFormats", res);
	int64_t format = 0;
	for (int64_t f : formats) {
		if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && format == 0))
			format = f;
	}
	if (format == 0) {
		OOVR_LOG("Helmet view: the runtime offers no 8-bit sRGB swapchain format, so it is off");
		unsupported = true;
		return false;
	}
	if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
		for (size_t i = 0; i < rgba.size(); i += 4)
			std::swap(rgba[i], rgba[i + 2]);
	}

	// One swapchain, remade only if a visor image has another size. Each change writes a new image into it, and
	// the runtime keeps showing the last one released.
	if (chain != XR_NULL_HANDLE && (chainWidth != width || chainHeight != height)) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}
	if (chain == XR_NULL_HANDLE) {
		XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		info.format = format;
		info.sampleCount = 1;
		info.width = width;
		info.height = height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		if (XR_FAILED(res = xrCreateSwapchain(xr_session.get(), &info, &chain))) {
			chain = XR_NULL_HANDLE;
			return fail("xrCreateSwapchain", res);
		}
		chainWidth = width;
		chainHeight = height;
	}

	uint32_t imageCount = 0;
	if (XR_FAILED(res = xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr)) || imageCount == 0)
		return fail("xrEnumerateSwapchainImages", res);
	std::vector<XrSwapchainImageD3D12KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
	if (XR_FAILED(res = xrEnumerateSwapchainImages(chain, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)images.data())))
		return fail("xrEnumerateSwapchainImages", res);

	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t index = 0;
	if (XR_FAILED(res = xrAcquireSwapchainImage(chain, &acquireInfo, &index)))
		return fail("xrAcquireSwapchainImage", res);
	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(res = xrWaitSwapchainImage(chain, &waitInfo)))
		return fail("xrWaitSwapchainImage", res);

	// Every acquired image must be released, whatever happens to the copy
	bool copied = false;
	HRESULT hr = S_OK;
	const char* step = "";
	{
		// Upload the pixels through a staging buffer on the game's own queue, then wait for the copy to finish
		ID3D12Device* device = binding->device;
		ID3D12CommandQueue* queue = binding->queue;
		ID3D12Resource* image = images[index].texture;

		D3D12_RESOURCE_DESC imageDesc = image->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
		UINT64 uploadSize = 0;
		device->GetCopyableFootprints(&imageDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadSize);

		D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
		D3D12_RESOURCE_DESC bufferDesc{};
		bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width = uploadSize;
		bufferDesc.Height = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels = 1;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		ComPtr<ID3D12Resource> upload;
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence> fence;
		unsigned char* mapped = nullptr;

		if (FAILED(hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
		        nullptr, IID_PPV_ARGS(&upload))))
			step = "CreateCommittedResource";
		else if (FAILED(hr = upload->Map(0, nullptr, (void**)&mapped)))
			step = "Map";
		else if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
		    || FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
		    || FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
			step = "creating the upload commands";
		else {
			for (unsigned y = 0; y < height; y++)
				memcpy(mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch, rgba.data() + (size_t)y * width * 4, (size_t)width * 4);
			upload->Unmap(0, nullptr);

			// OpenXR hands out D3D12 colour images in the render target state and wants them back in it
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = image;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			list->ResourceBarrier(1, &barrier);

			D3D12_TEXTURE_COPY_LOCATION dst{ image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			dst.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			src.PlacedFootprint = footprint;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			list->ResourceBarrier(1, &barrier);
			list->Close();

			ID3D12CommandList* lists[] = { list.Get() };
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence.Get(), 1);
			if (fence->GetCompletedValue() < 1) {
				HANDLE done = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				fence->SetEventOnCompletion(1, done);
				WaitForSingleObject(done, INFINITE);
				CloseHandle(done);
			}
			copied = true;
		}
	}

	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	if (XR_FAILED(res = xrReleaseSwapchainImage(chain, &releaseInfo)))
		return fail("xrReleaseSwapchainImage", res);
	if (!copied)
		return fail(step, hr);

	// Head-locked plane in front of the eyes, covering the whole view; the PNG has straight (not premultiplied) alpha
	const float halfSpan = kHelmetDistance * std::tan(kHelmetHalfAngleDeg * 3.14159265f / 180.0f);
	layer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = chain;
	layer.subImage.imageRect = { { 0, 0 }, { (int32_t)width, (int32_t)height } };
	layer.subImage.imageArrayIndex = 0;
	layer.pose = { { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, -kHelmetDistance } };
	layer.size = { 2.0f * halfSpan, 2.0f * halfSpan * (float)height / (float)width };

	OOVR_LOGF("Helmet view: visor %s (%ux%u image, format %lld)", visor.c_str(), width, height, (long long)format);
	return true;
}

#else

bool HelmetView::Show(const void*, const std::string&)
{
	OOVR_LOG("Helmet view: this build has no Direct3D 12 support, so it is off");
	unsupported = true;
	return false;
}

#endif
