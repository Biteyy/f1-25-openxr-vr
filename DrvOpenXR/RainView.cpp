#include "RainView.h"

#include "AddonSettings.h"

#include <algorithm>

// The visor drops' part of the swapchain, square, in pixels
static const uint32_t kDropSize = 768;

// How far away the drops' quad is drawn: far enough that both eyes see the same image, so the drops sit on the view
// itself like drops on a lens. At arm's length (RainSim's plane) they read as a bubble floating around the head.
static const float kDropLayerDistance = 20.0f;

RainView::~RainView()
{
	Reset();
}

void RainView::Reset()
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	draw.Release(); // waits for the GPU to finish with the swapchain images
#endif
	if (chain != XR_NULL_HANDLE) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}
	images.clear();
	chainWidth = chainHeight = eyeWidth = eyeHeight = 0;
	unsupported = false;
}

void RainView::AddLayers(const void* graphicsBinding, const XrCompositionLayerProjectionView* gameViews, XrSpace space,
    std::vector<const XrCompositionLayerBaseHeader*>& layers)
{
	if (unsupported)
		return;

	auto now = std::chrono::steady_clock::now();
	float dt = last.time_since_epoch().count() != 0 ? std::chrono::duration<float>(now - last).count() : 0.0f;
	last = now;
	double seconds = std::chrono::duration<double>(now - start).count();

	Rain::Feed feed{};
	feed.size = sizeof(feed);
	bool have = AddonSettings::RainFeed(feed);
	sim.Update(have ? &feed : nullptr, seconds, dt);
	if (!sim.Active())
		return;
	sim.Build(spray, drops);
	if (spray.empty() && drops.empty())
		return;

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	const auto* binding = (const XrGraphicsBindingD3D12KHR*)graphicsBinding;
	if (!binding || binding->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
		OOVR_LOG("Rain: only Direct3D 12 games are supported, so no spray or visor drops");
		unsupported = true;
		return;
	}

	// Half the game's resolution per axis: spray is soft, and it keeps the cost down
	uint32_t eyeW = std::max<uint32_t>(256, (uint32_t)gameViews[0].subImage.imageRect.extent.width / 2);
	uint32_t eyeH = std::max<uint32_t>(256, (uint32_t)gameViews[0].subImage.imageRect.extent.height / 2);
	if (!MakeSwapchain(eyeW, eyeH))
		return;

	XrResult res;
	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t index = 0;
	if (XR_FAILED(res = xrAcquireSwapchainImage(chain, &acquireInfo, &index))) {
		OOVR_LOGF("Rain: xrAcquireSwapchainImage failed (%d)", res);
		return;
	}
	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	bool drawn = false;
	if (XR_SUCCEEDED(res = xrWaitSwapchainImage(chain, &waitInfo))) {
		RainDraw::Pass passes[3];
		for (int eye = 0; eye < 2; eye++) {
			const XrPosef& pose = gameViews[eye].pose;
			const XrFovf& fov = gameViews[eye].fov;
			float q[4] = { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w };
			float p[3] = { pose.position.x, pose.position.y, pose.position.z };
			RainDraw::EyeCamera(q, p, fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown, passes[eye]);
			passes[eye].rect = { (LONG)(eye * eyeWidth), 0, (LONG)((eye + 1) * eyeWidth), (LONG)eyeHeight };
			passes[eye].sprites = &spray;
		}
		RainDraw::PlaneCamera(Rain::kVisorHalfSize, passes[2]);
		passes[2].rect = { (LONG)(2 * eyeWidth), 0, (LONG)(2 * eyeWidth + dropSize), (LONG)dropSize };
		passes[2].sprites = &drops;
		drawn = draw.Render(binding->device, binding->queue, (ID3D12Resource*)images[index], (DXGI_FORMAT)chainFormat, passes, 3);
	} else {
		OOVR_LOGF("Rain: xrWaitSwapchainImage failed (%d)", res);
	}
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(chain, &releaseInfo);
	if (!drawn)
		return;

	if (!loggedActive) {
		OOVR_LOGF("Rain: drawing spray and visor drops (track wetness %.2f, %ux%u per eye)", sim.Wetness(), eyeWidth, eyeHeight);
		loggedActive = true;
	}

	// The spray: seen from where the game drew its frame, so it sits in the same world
	if (!spray.empty()) {
		for (int eye = 0; eye < 2; eye++) {
			views[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
			views[eye].pose = gameViews[eye].pose;
			views[eye].fov = gameViews[eye].fov;
			views[eye].subImage.swapchain = chain;
			views[eye].subImage.imageRect = { { (int32_t)(eye * eyeWidth), 0 }, { (int32_t)eyeWidth, (int32_t)eyeHeight } };
			views[eye].subImage.imageArrayIndex = 0;
		}
		sprayLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		sprayLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		sprayLayer.space = space;
		sprayLayer.viewCount = 2;
		sprayLayer.views = views;
		layers.push_back((const XrCompositionLayerBaseHeader*)&sprayLayer);
	}

	// The drops: on the visor, moving with the head
	if (!drops.empty()) {
		dropLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		dropLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		dropLayer.space = xr_gbl->viewSpace;
		dropLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		dropLayer.subImage.swapchain = chain;
		dropLayer.subImage.imageRect = { { (int32_t)(2 * eyeWidth), 0 }, { (int32_t)dropSize, (int32_t)dropSize } };
		dropLayer.subImage.imageArrayIndex = 0;
		// The same angles as RainSim's plane, scaled out to kDropLayerDistance
		const float scale = kDropLayerDistance / Rain::kVisorDistance;
		dropLayer.pose = { { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, -kDropLayerDistance } };
		dropLayer.size = { 2.0f * Rain::kVisorHalfSize * scale, 2.0f * Rain::kVisorHalfSize * scale };
		layers.push_back((const XrCompositionLayerBaseHeader*)&dropLayer);
	}
#else
	(void)graphicsBinding, (void)gameViews, (void)space, (void)layers;
	OOVR_LOG("Rain: this build has no Direct3D 12 support, so no spray or visor drops");
	unsupported = true;
#endif
}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

bool RainView::MakeSwapchain(uint32_t eyeW, uint32_t eyeH)
{
	if (chain != XR_NULL_HANDLE && eyeW == eyeWidth && eyeH == eyeHeight)
		return true;
	if (chain != XR_NULL_HANDLE) {
		draw.Release(); // the old images may still be in use
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
		images.clear();
	}

	auto fail = [this](const char* what, XrResult res) {
		OOVR_LOGF("Rain: %s failed (%d), so no spray or visor drops", what, res);
		unsupported = true;
		return false;
	};

	// An 8-bit sRGB format with alpha, as the helmet view uses
	uint32_t count = 0;
	XrResult res;
	if (XR_FAILED(res = xrEnumerateSwapchainFormats(xr_session.get(), 0, &count, nullptr)))
		return fail("xrEnumerateSwapchainFormats", res);
	std::vector<int64_t> formats(count);
	if (XR_FAILED(res = xrEnumerateSwapchainFormats(xr_session.get(), count, &count, formats.data())))
		return fail("xrEnumerateSwapchainFormats", res);
	int64_t format = 0;
	for (int64_t f : formats)
		if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && format == 0))
			format = f;
	if (format == 0)
		return fail("finding an 8-bit sRGB swapchain format", XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);

	XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = format;
	info.sampleCount = 1;
	info.width = 2 * eyeW + kDropSize;
	info.height = std::max(eyeH, kDropSize);
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (XR_FAILED(res = xrCreateSwapchain(xr_session.get(), &info, &chain))) {
		chain = XR_NULL_HANDLE;
		return fail("xrCreateSwapchain", res);
	}

	uint32_t imageCount = 0;
	if (XR_FAILED(res = xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr)) || imageCount == 0)
		return fail("xrEnumerateSwapchainImages", res);
	std::vector<XrSwapchainImageD3D12KHR> list(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
	if (XR_FAILED(res = xrEnumerateSwapchainImages(chain, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)list.data())))
		return fail("xrEnumerateSwapchainImages", res);
	images.clear();
	for (const auto& image : list)
		images.push_back(image.texture);

	chainFormat = format;
	chainWidth = info.width;
	chainHeight = info.height;
	eyeWidth = eyeW;
	eyeHeight = eyeH;
	dropSize = kDropSize;
	return true;
}

#endif
