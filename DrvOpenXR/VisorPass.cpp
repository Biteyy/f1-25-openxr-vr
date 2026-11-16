#include "VisorPass.h"

#if defined(_WIN32)

#include "VisorLook.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef RAIN_STANDALONE
#include <cstdio>
#define VISOR_LOG(...) (printf(__VA_ARGS__), printf("\n"))
#else
#include "../OpenOVR/logging.h"
#define VISOR_LOG(...) OOVR_LOGF(__VA_ARGS__)
#endif

namespace {

// The visor map spans +-this angle from straight ahead (on a flat plane), well past any headset's view
const float kVisorHalfAngleDeg = 80.0f;
const unsigned kVisorMapSize = 512;

// The drops' normal map, square, in the layers texture next to the spray
const UINT kWaterSize = 768;
const DXGI_FORMAT kLayersFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

const UINT kConstants = 32;

// One full-screen triangle. Colours are linear: the scene is read and the swapchain written through typed (sRGB
// where the game's image is 8-bit) views.
const char kShaders[] = R"(
cbuffer C : register(b0) {
	float4 fov;    // tangents: left, right, up, down
	float4 plane;  // visor plane distance and half size (metres), the visor map's span (tangent), visor on
	float4 wet;    // layers on, veil, wetness, fog
	float4 film;   // the outer tear-off's dirt, grime, seed; tear-offs on the visor
	float4 pull;   // a tear-off being pulled (0/1), how far it has slid (metres), its dirt, its grime
	float4 spray;  // this eye's spray in the layers: u offset, u scale, v scale; the pulled tear-off's seed
	float4 water;  // the drops in the layers: u offset, u scale, v scale; time
	float4 misc;   // output width, height; speed (m/s); photochromic
};
Texture2D scene : register(t0);
Texture2D layers : register(t1);
Texture2D visorMap : register(t2);
SamplerState lin : register(s0);

float4 vs(uint id : SV_VertexID) : SV_Position {
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

// 0..1 from a cell's whole-number coordinates
float hash(float2 p) {
	uint2 q = asuint(int2(floor(p))) * uint2(1597334673u, 3812015801u);
	uint n = (q.x ^ q.y) * 1597334673u;
	n ^= n >> 16;
	return float(n & 0xffffff) / 16777216.0;
}
float noise(float2 p) {
	float2 i = floor(p), f = frac(p);
	f = f * f * (3 - 2 * f);
	return lerp(lerp(hash(i), hash(i + float2(1, 0)), f.x), lerp(hash(i + float2(0, 1)), hash(i + 1), f.x), f.y);
}

// Soft dots, one at most per cell (metres across), in a share of the cells that grows with amount
float specks(float2 p, float cell, float amount, float seed) {
	float2 g = p / cell + seed * 37.0;
	float2 i = floor(g), f = frac(g);
	float h = hash(i);
	if (h >= amount)
		return 0;
	float2 c = float2(hash(i + 1013), hash(i + 2027)) * 0.6 + 0.2;
	float r = 0.14 + 0.22 * hash(i + 3049);
	return (1 - smoothstep(r * 0.25, r, length(f - c))) * (0.5 + 0.5 * hash(i + 4051));
}

// A tear-off at visor-plane point p: rubber and dust specks (out of focus, so soft), now and then a bug, the spray's
// grime streaked out and up by the air, and the film's faint sheen
float3 tearOff(float3 c, float2 p, float dirt, float grime, float seed, float up, float3 sky) {
	float d = specks(p, 0.006, dirt * 0.30, seed) * 0.55 + specks(p, 0.013, dirt * 0.12, seed + 3.0) * 0.75;
	c *= 1 - saturate(d) * 0.8;
	float bug = specks(p, 0.06, saturate(dirt - 0.4) * 0.10, seed + 7.0);
	c = lerp(c, sky * float3(0.35, 0.32, 0.22), bug * 0.6);
	float streak = noise(float2(p.x * 9.0 + seed * 5.0, p.y * 30.0 - abs(p.x) * 6.0)) * 0.6 + noise(p * 22.0 + seed) * 0.4;
	c = lerp(c, sky * float3(0.62, 0.60, 0.56), saturate(grime * (0.55 + 0.6 * streak)) * 0.35);
	return c + sky * 0.012 * saturate(0.5 + up);
}

float4 ps(float4 pos : SV_Position) : SV_Target {
	float2 uv = pos.xy / misc.xy;
	float2 t = float2(lerp(fov.x, fov.y, uv.x), lerp(fov.z, fov.w, uv.y)); // direction: tangents right, up
	float2 p = t * plane.x; // on the visor plane, metres
	float2 du = float2(1.0 / (fov.y - fov.x), -1.0 / (fov.z - fov.w)); // image distance per tangent

	// The light from the sky: a few points at the top of the view, the darkest left out (the halo may hide one),
	// kept to the range of a lit sky so the sun itself doesn't light up the marks on the visor
	float3 sky = 0, darkest = 100;
	for (int k = 0; k < 5; k++) {
		float3 s = min(scene.SampleLevel(lin, float2(0.15 + 0.175 * k, 0.1), 0).rgb, 1.0);
		sky += s;
		darkest = dot(s, 1) < dot(darkest, 1) ? s : darkest;
	}
	sky = clamp((sky - darkest) * 0.25, 0.04, 0.8);

	float3 c = scene.SampleLevel(lin, uv, 0).rgb;

	// The spray (premultiplied), then the veil inside a cloud: everything grey and flat
	if (wet.x > 0.5) {
		float4 s = layers.SampleLevel(lin, float2(spray.x + uv.x * spray.y, uv.y * spray.z), 0);
		c = c * (1 - s.a) + s.rgb;
	}
	float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
	c = lerp(c, lerp(c, luma.xxx, 0.5) * 0.55 + sky * float3(0.55, 0.58, 0.62), wet.y);

	// The drops: lenses showing the world from a wider angle, upside down and blurred (out of focus), darker at the
	// steep rim where light leaks out, with the sky's highlight on top
	if (wet.x > 0.5) {
		float2 wuv = float2(water.x + (p.x / plane.y * 0.5 + 0.5) * water.y, (0.5 - p.y / plane.y * 0.5) * water.z);
		float4 w = layers.SampleLevel(lin, wuv, 0);
		if (w.a > 0.003) {
			float cover = saturate(w.a);
			float2 n = w.rg / w.a * 2 - 1;
			float thick = saturate(w.b / w.a);
			float2 at = uv - n * (0.18 + 0.30 * thick) * du;
			float3 r = scene.SampleLevel(lin, at, 0).rgb;
			r += scene.SampleLevel(lin, at + float2(0.006, 0.004), 0).rgb;
			r += scene.SampleLevel(lin, at + float2(-0.005, 0.006), 0).rgb;
			r += scene.SampleLevel(lin, at + float2(0.003, -0.007), 0).rgb;
			r *= 0.25;
			float rim = saturate(dot(n, n));
			r *= lerp(0.95, 0.45, rim * rim);
			float glint = pow(saturate(dot(normalize(float3(n, 0.55)), normalize(float3(-0.25, 0.75, 0.6)))), 30.0);
			r += sky * glint * 0.9;
			c = lerp(c, r, cover);
		}
	}

	// The tear-offs. One being pulled slides off to the left: left of its right edge it still covers the visor, with
	// its edge curling up off it (a soft bright line, bowed, bending the view a little); right of it, the next one
	// shows.
	float edge = plane.y - pull.y - 0.05 * p.y * p.y;
	if (pull.x > 0.5) {
		float curl = saturate(1 - abs(p.x - edge) / 0.02);
		c = lerp(c, scene.SampleLevel(lin, uv + float2(0.004, 0) * curl, 0).rgb, curl * 0.5);
		c += sky * curl * curl * 0.18;
	}
	if (pull.x > 0.5 && p.x < edge) {
		c = tearOff(c, p + float2(pull.y, 0), pull.z, pull.w, spray.w, t.y, sky);
	} else if (film.w > 0.5) {
		c = tearOff(c, p, film.x, film.y, film.z, t.y, sky);
	}
	// The stack's edges near the far left, out of focus: one faint line per tear-off left, fading out up and down
	float lines = 0;
	for (int i = 0; i < 6; i++)
		if (i < film.w)
			lines += saturate(1 - abs(t.x - (-1.12 - 0.012 * i)) / 0.006);
	c += sky * 0.035 * lines * saturate(1 - abs(t.y + 0.15) / 0.55);

	// Misting at the bottom, uneven
	float mist = saturate((-t.y - 0.25) / 0.45) * wet.w * (0.65 + 0.2 * noise(p * 12.0) + 0.15 * noise(p * 31.0));
	c = lerp(c, sky * 0.7 + c * 0.3, saturate(mist));

	// The visor's tint, then the helmet round the opening: near black, a shade lighter at its edge where light from
	// the opening falls on it
	if (plane.w > 0.5) {
		float4 m = visorMap.SampleLevel(lin, float2(t.x / plane.z * 0.5 + 0.5, 0.5 - t.y / plane.z * 0.5), 0);
		c *= m.rgb;
		// A photochromic visor darkens with the light: clear at dusk and in tunnels, about a dark smoke in sun
		if (misc.w > 0.5)
			c *= lerp(0.92, 0.36, smoothstep(0.12, 0.55, dot(sky, 1.0 / 3.0)));
		float lining = 0.010 + 0.06 * m.a * (1 - m.a) * dot(sky, 0.33);
		c = lerp(c, lining.xxx, m.a);
	}
	return float4(c, 1);
}
)";

typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* SerializeFn)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

// The format family a texture can be copied within, as a typeless format
DXGI_FORMAT TypelessOf(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = r;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = from;
	b.Transition.StateAfter = to;
	list->ResourceBarrier(1, &b);
}

} // namespace

VisorPass::~VisorPass()
{
	Release();
}

void VisorPass::WaitIdle()
{
	if (fence && lastQueue && fenceCounter > 0 && fence->GetCompletedValue() < fenceCounter) {
		fence->SetEventOnCompletion(fenceCounter, fenceEvent);
		WaitForSingleObject(fenceEvent, 5000);
	}
}

void VisorPass::Release()
{
	WaitIdle();
	draw.Release();
	for (Slot& s : slots)
		s = Slot();
	for (int i = 0; i < kFormats; i++) {
		pipelines[i].Reset();
		pipelineFormats[i] = DXGI_FORMAT_UNKNOWN;
	}
	root.Reset();
	vs.Reset();
	ps.Reset();
	srvHeap.Reset();
	rtvHeap.Reset();
	scene[0].Reset();
	scene[1].Reset();
	layers.Reset();
	visorMap.Reset();
	mirror.Reset();
	fence.Reset();
	fenceCounter = 0;
	if (fenceEvent) {
		CloseHandle(fenceEvent);
		fenceEvent = nullptr;
	}
	device = nullptr;
	lastQueue = nullptr;
	failed = false;
	eyeWidth = eyeHeight = 0;
	eyeFormat = DXGI_FORMAT_UNKNOWN;
	layersAsTarget = true;
	layersDrawn = mirrorReady = false;
	visorGeneration = ~0u;
	visorOn = false;
}

bool VisorPass::Setup(ID3D12Device* dev)
{
	device = dev;
	auto fail = [this](const char* what, HRESULT hr) {
		VISOR_LOG("Visor pass: %s failed (0x%08lx), so the game's image goes to the headset as it is", what, (unsigned long)hr);
		failed = true;
		return false;
	};

	HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	CompileFn compile = compiler ? (CompileFn)GetProcAddress(compiler, "D3DCompile") : nullptr;
	HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
	SerializeFn serialize = d3d12 ? (SerializeFn)GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr;
	if (!compile || !serialize)
		return fail("loading the shader compiler", E_FAIL);
	ComPtr<ID3DBlob> errors;
	HRESULT hr = compile(kShaders, sizeof(kShaders) - 1, "visor", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors);
	if (SUCCEEDED(hr))
		hr = compile(kShaders, sizeof(kShaders) - 1, "visor", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
	if (FAILED(hr)) {
		if (errors)
			VISOR_LOG("Visor pass: shader errors: %.*s", (int)errors->GetBufferSize(), (const char*)errors->GetBufferPointer());
		return fail("compiling the shaders", hr);
	}

	D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants = { 0, 0, kConstants };
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable = { 1, &range };
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rsDesc{ 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
	ComPtr<ID3DBlob> rs;
	if (FAILED(hr = serialize(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &errors)))
		return fail("D3D12SerializeRootSignature", hr);
	if (FAILED(hr = device->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root))))
		return fail("CreateRootSignature", hr);

	D3D12_DESCRIPTOR_HEAP_DESC heap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
	if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srvHeap))))
		return fail("CreateDescriptorHeap", hr);
	heap = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
	if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap))))
		return fail("CreateDescriptorHeap", hr);
	srvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		return fail("CreateFence", hr);
	fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	for (Slot& s : slots) {
		if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))))
			return fail("CreateCommandAllocator", hr);
		if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.list))))
			return fail("CreateCommandList", hr);
		s.list->Close();
		if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.pre))))
			return fail("CreateCommandList", hr);
		s.pre->Close();
	}
	return true;
}

ID3D12PipelineState* VisorPass::PipelineFor(DXGI_FORMAT format)
{
	int slot = -1;
	for (int i = 0; i < kFormats; i++) {
		if (pipelines[i] && pipelineFormats[i] == format)
			return pipelines[i].Get();
		if (!pipelines[i] && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		pipelines[0].Reset(); // a third format: start the cache over
		slot = 0;
	}
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = root.Get();
	desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = format;
	desc.SampleDesc.Count = 1;
	HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[slot]));
	if (FAILED(hr)) {
		VISOR_LOG("Visor pass: CreateGraphicsPipelineState failed (0x%08lx) for format %d, so the game's image goes to the headset as it is", (unsigned long)hr, (int)format);
		failed = true;
		return nullptr;
	}
	pipelineFormats[slot] = format;
	return pipelines[slot].Get();
}

bool VisorPass::MakeTargets(const D3D12_RESOURCE_DESC& src, DXGI_FORMAT format)
{
	WaitIdle();
	scene[0].Reset();
	scene[1].Reset();
	layers.Reset();
	mirror.Reset();
	mirrorReady = false;

	DXGI_FORMAT family = TypelessOf(format);
	if (family == DXGI_FORMAT_UNKNOWN || TypelessOf(src.Format) != family) {
		VISOR_LOG("Visor pass: the eye image's format %d (swapchain %d) isn't one it knows, so the game's image goes to the headset as it is", (int)src.Format, (int)format);
		failed = true;
		return false;
	}

	D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC tex{};
	tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	tex.Width = src.Width;
	tex.Height = src.Height;
	tex.DepthOrArraySize = 1;
	tex.MipLevels = 1;
	tex.Format = family;
	tex.SampleDesc.Count = 1;
	HRESULT hr = S_OK;
	for (int i = 0; i < 2 && SUCCEEDED(hr); i++)
		hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&scene[i]));
	if (SUCCEEDED(hr))
		hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&mirror));

	// The layers: both eyes' spray at half resolution side by side, then the drops' square
	sprayWidth = std::max<UINT>(256, (UINT)(src.Width / 2));
	sprayHeight = std::max<UINT>(256, src.Height / 2);
	layersWidth = 2 * sprayWidth + kWaterSize;
	layersHeight = std::max(sprayHeight, kWaterSize);
	tex.Width = layersWidth;
	tex.Height = layersHeight;
	tex.Format = kLayersFormat;
	tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_CLEAR_VALUE clear{ kLayersFormat, { 0, 0, 0, 0 } };
	if (SUCCEEDED(hr))
		hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&layers));
	if (FAILED(hr)) {
		VISOR_LOG("Visor pass: making its textures failed (0x%08lx), so the game's image goes to the headset as it is", (unsigned long)hr);
		failed = true;
		return false;
	}
	layersAsTarget = true;
	layersDrawn = false;

	// Views: per eye its scene, the layers, the visor map (null until one is uploaded)
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	for (int eye = 0; eye < 2; eye++) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += (SIZE_T)srvStep * 3 * eye;
		srv.Format = format;
		device->CreateShaderResourceView(scene[eye].Get(), &srv, h);
		h.ptr += srvStep;
		srv.Format = kLayersFormat;
		device->CreateShaderResourceView(layers.Get(), &srv, h);
		h.ptr += srvStep;
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		device->CreateShaderResourceView(visorMap.Get(), &srv, h); // null is fine
	}
	eyeWidth = src.Width;
	eyeHeight = src.Height;
	eyeFormat = format;
	VISOR_LOG("Visor pass: on (eye %llux%u, format %d, spray %ux%u)", (unsigned long long)src.Width, src.Height, (int)format, sprayWidth, sprayHeight);
	return true;
}

bool VisorPass::UploadVisor(ID3D12CommandQueue* queue, const std::string& id)
{
	visorOn = false;
	visorPhotochromic = false;
	VisorLook::Visor look;
	if (id == "off")
		return true;
	// The id comes from the addon's settings: only the visors VisorLook knows
	if (!VisorLook::Find(id, look)) {
		VISOR_LOG("Visor pass: unknown visor '%.40s', so none", id.c_str());
		return true;
	}
	std::vector<unsigned char> rgba;
	VisorLook::Build(look, kVisorMapSize, kVisorHalfAngleDeg, rgba);

	WaitIdle(); // the old map may still be in use
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = desc.Height = kVisorMapSize;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
	UINT64 total = 0;
	device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
	D3D12_RESOURCE_DESC buf{};
	buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buf.Width = total;
	buf.Height = buf.DepthOrArraySize = buf.MipLevels = 1;
	buf.SampleDesc.Count = 1;
	buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT }, uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
	ComPtr<ID3D12Resource> texture, upload;
	unsigned char* mapped = nullptr;
	HRESULT hr;
	if (FAILED(hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)))
	    || FAILED(hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))
	    || FAILED(hr = upload->Map(0, nullptr, (void**)&mapped))) {
		VISOR_LOG("Visor pass: uploading the visor failed (0x%08lx), so none", (unsigned long)hr);
		return true;
	}
	for (unsigned y = 0; y < kVisorMapSize; y++)
		memcpy(mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch, rgba.data() + (size_t)y * kVisorMapSize * 4, (size_t)kVisorMapSize * 4);
	upload->Unmap(0, nullptr);

	Slot& s = slots[nextSlot];
	nextSlot = (nextSlot + 1) % kSlots;
	if (fence->GetCompletedValue() < s.fenceValue) {
		fence->SetEventOnCompletion(s.fenceValue, fenceEvent);
		WaitForSingleObject(fenceEvent, 2000);
	}
	s.allocator->Reset();
	s.list->Reset(s.allocator.Get(), nullptr);
	D3D12_TEXTURE_COPY_LOCATION dst{ texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION from{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
	from.PlacedFootprint = fp;
	s.list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
	Transition(s.list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	s.list->Close();
	ID3D12CommandList* lists[] = { s.list.Get() };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.Get(), ++fenceCounter);
	s.fenceValue = fenceCounter;
	lastQueue = queue;
	WaitIdle(); // once per visor change, so the upload memory can go

	visorMap = texture;
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	for (int eye = 0; eye < 2; eye++) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += (SIZE_T)srvStep * (3 * eye + 2);
		device->CreateShaderResourceView(visorMap.Get(), &srv, h);
	}
	visorOn = true;
	visorPhotochromic = look.photochromic;
	VISOR_LOG("Visor pass: visor %s (lets through %.2f %.2f %.2f)", id.c_str(), look.transmission[0], look.transmission[1], look.transmission[2]);
	return true;
}

void VisorPass::Step(const FrameInput& input)
{
	auto now = std::chrono::steady_clock::now();
	float dt = input.dt;
	if (dt < 0)
		dt = last.time_since_epoch().count() != 0 ? std::chrono::duration<float>(now - last).count() : 0.0f;
	last = now;
	time += dt;
	double seconds = input.dt < 0 ? std::chrono::duration<double>(now - start).count() : (double)time;
	sim.Update(input.feed, seconds, dt);
	sim.Build(spray, drops);
	state = sim.Visor();

	if (!loggedRain && (!spray.empty() || !drops.empty())) {
		VISOR_LOG("Visor pass: rain on the visor (track wetness %.2f)", sim.Wetness());
		loggedRain = true;
	}
	uint32_t pulled = sim.TearOffsPulled();
	if (pulled != loggedTearOffs) {
		if (pulled > loggedTearOffs)
			VISOR_LOG("Visor pass: tear-off pulled (%u of %u this session)", pulled, Rain::Sim::kTearOffs);
		loggedTearOffs = pulled;
	}
}

bool VisorPass::Draw(int eye, ID3D12CommandQueue* queue, ID3D12Resource* src, ID3D12Resource* dst, DXGI_FORMAT format,
    const EyeView views[2], const FrameInput& input)
{
	if (eye < 0 || eye > 1 || !queue || !src || !dst)
		return false;
	ComPtr<ID3D12Device> dev;
	if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev))))
		return false;
	if (device && dev.Get() != device)
		Release(); // a new device: start over
	if (failed)
		return false;
	if (!device && !Setup(dev.Get()))
		return false;
	lastQueue = queue;

	D3D12_RESOURCE_DESC srcDesc = src->GetDesc();
	if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || srcDesc.MipLevels != 1 || srcDesc.DepthOrArraySize != 1 || srcDesc.SampleDesc.Count != 1) {
		VISOR_LOG("Visor pass: the eye image isn't a plain 2D texture (%u mips, %u layers, %u samples), so the game's image goes to the headset as it is",
		    srcDesc.MipLevels, srcDesc.DepthOrArraySize, srcDesc.SampleDesc.Count);
		failed = true;
		return false;
	}
	if (!scene[0] || srcDesc.Width != eyeWidth || srcDesc.Height != eyeHeight || format != eyeFormat) {
		if (!MakeTargets(srcDesc, format))
			return false;
	}
	ID3D12PipelineState* pipeline = PipelineFor(format);
	if (!pipeline)
		return false;

	Slot& s = slots[nextSlot];
	nextSlot = (nextSlot + 1) % kSlots;
	if (fence->GetCompletedValue() < s.fenceValue) {
		fence->SetEventOnCompletion(s.fenceValue, fenceEvent);
		WaitForSingleObject(fenceEvent, 1000);
	}
	s.allocator->Reset();

	// The left eye comes first each frame: step the rain and the visor, and draw both eyes' spray and the drops
	if (eye == 0) {
		if (input.visorGeneration != visorGeneration) {
			visorGeneration = input.visorGeneration;
			UploadVisor(queue, input.visor);
		}
		Step(input);
		layersDrawn = false;
		if (!spray.empty() || !drops.empty()) {
			if (!layersAsTarget) {
				s.pre->Reset(s.allocator.Get(), nullptr);
				Transition(s.pre.Get(), layers.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
				s.pre->Close();
				ID3D12CommandList* pre[] = { s.pre.Get() };
				queue->ExecuteCommandLists(1, pre);
				layersAsTarget = true;
			}
			RainDraw::Pass passes[3];
			for (int e = 0; e < 2; e++) {
				const EyeView& v = views[e];
				RainDraw::EyeCamera(v.orientation, v.position, v.angleLeft, v.angleRight, v.angleUp, v.angleDown, passes[e]);
				passes[e].rect = { (LONG)(e * sprayWidth), 0, (LONG)((e + 1) * sprayWidth), (LONG)sprayHeight };
				passes[e].sprites = &spray;
			}
			RainDraw::PlaneCamera(Rain::kVisorHalfSize, passes[2]);
			passes[2].rect = { (LONG)(2 * sprayWidth), 0, (LONG)(2 * sprayWidth + kWaterSize), (LONG)kWaterSize };
			passes[2].sprites = &drops;
			layersDrawn = draw.Render(device, queue, layers.Get(), kLayersFormat, passes, 3, true);
		}
	}

	ID3D12GraphicsCommandList* list = s.list.Get();
	list->Reset(s.allocator.Get(), pipeline);
	if (layersDrawn && layersAsTarget) {
		Transition(list, layers.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		layersAsTarget = false;
	}

	// The game's image, read as the plain copy reads it (no barrier of its own)
	list->CopyResource(scene[eye].Get(), src);
	Transition(list, scene[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = format;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(dst, &rtvDesc, rtv);
	list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	D3D12_VIEWPORT viewport{ 0, 0, (float)eyeWidth, (float)eyeHeight, 0, 1 };
	D3D12_RECT scissor{ 0, 0, (LONG)eyeWidth, (LONG)eyeHeight };
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	list->SetGraphicsRootSignature(root.Get());
	ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
	list->SetDescriptorHeaps(1, heaps);
	D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeap->GetGPUDescriptorHandleForHeapStart();
	table.ptr += (UINT64)srvStep * 3 * eye;
	list->SetGraphicsRootDescriptorTable(1, table);

	const EyeView& v = views[eye];
	const Rain::VisorState& st = state;
	float pullShift = st.peel > 0 ? 2.3f * Rain::kVisorHalfSize * st.peel * st.peel : 0.0f; // as RainSim slides it
	float c[kConstants] = {
		std::tan(v.angleLeft), std::tan(v.angleRight), std::tan(v.angleUp), std::tan(v.angleDown),
		Rain::kVisorDistance, Rain::kVisorHalfSize, std::tan(kVisorHalfAngleDeg * 3.14159265f / 180.0f), visorOn ? 1.0f : 0.0f,
		layersDrawn ? 1.0f : 0.0f, st.veil, st.wetness, st.fog,
		st.dirt, st.grime, st.seed, (float)st.layersLeft,
		st.peel > 0 ? 1.0f : 0.0f, pullShift, st.oldDirt, st.oldGrime,
		(float)(eye * sprayWidth) / layersWidth, (float)sprayWidth / layersWidth, (float)sprayHeight / layersHeight, st.oldSeed,
		(float)(2 * sprayWidth) / layersWidth, (float)kWaterSize / layersWidth, (float)kWaterSize / layersHeight, time,
		(float)eyeWidth, (float)eyeHeight, st.speed, visorPhotochromic ? 1.0f : 0.0f,
	};
	list->SetGraphicsRoot32BitConstants(0, kConstants, c, 0);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	list->DrawInstanced(3, 1, 0, 0);

	Transition(list, scene[eye].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

	// The left eye as the headset shows it, for the VR view window
	if (eye == 0) {
		mirrorReady = false;
		if (input.mirror) {
			Transition(list, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(list, mirror.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyResource(mirror.Get(), dst);
			Transition(list, mirror.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(list, dst, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			mirrorReady = true;
		}
	}

	if (FAILED(list->Close())) {
		VISOR_LOG("Visor pass: recording its commands failed, so the game's image goes to the headset as it is");
		failed = true;
		return false;
	}
	ID3D12CommandList* lists[] = { list };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.Get(), ++fenceCounter);
	s.fenceValue = fenceCounter;
	return true;
}

#endif
