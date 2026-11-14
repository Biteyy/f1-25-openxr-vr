#include "RainDraw.h"

#if defined(_WIN32)

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef RAIN_STANDALONE
#include <cstdio>
#define RAIN_LOG(...) (printf(__VA_ARGS__), printf("\n"))
#else
#include "../OpenOVR/logging.h"
#define RAIN_LOG(...) OOVR_LOGF(__VA_ARGS__)
#endif

namespace {

// Atlas: one 128x128 slice per tile, with mips so far-off puffs don't sparkle
const UINT kTileSize = 128;
const UINT kTiles = 3;
const UINT kMips = 8;

// Upload memory per frame: constants for a few passes, then the sprites
const UINT64 kUploadSize = 1u << 20;
const UINT kConstantsSize = 256; // per pass, the constant buffer alignment
const UINT kMaxPasses = 4;

// The spray's colour (linear), lit by a grey sky
const float kMistColour[3] = { 0.42f, 0.44f, 0.46f };

struct Instance {
	float pos[3];
	float half[2];
	float angle;
	float alpha;
	uint32_t tile;
};

struct Constants {
	float viewProj[16];
	float right[4];
	float up[4];
	float tint[4];
};

const char kShaders[] = R"(
cbuffer PassConstants : register(b0) {
	row_major float4x4 viewProj;
	float4 camRight;
	float4 camUp;
	float4 tint;
};
Texture2DArray atlas : register(t0);
SamplerState linearClamp : register(s0);

struct VIn {
	float3 pos : POS;
	float2 halfSize : HALF;
	float angle : ANGLE;
	float alpha : ALPHA;
	uint tile : TILE;
	uint vertex : SV_VertexID;
};
struct VOut {
	float4 pos : SV_Position;
	float2 uv : UV;
	float alpha : ALPHA;
	nointerpolation uint tile : TILE;
};

VOut vs(VIn i) {
	// A triangle strip: top left, top right, bottom left, bottom right
	float2 corner = float2((i.vertex & 1) ? 1.0 : -1.0, (i.vertex & 2) ? -1.0 : 1.0);
	float2 local = corner * i.halfSize;
	float s, c;
	sincos(i.angle, s, c);
	float2 turned = float2(c * local.x - s * local.y, s * local.x + c * local.y);
	float3 world = i.pos + camRight.xyz * turned.x + camUp.xyz * turned.y;
	VOut o;
	o.pos = mul(viewProj, float4(world, 1.0));
	o.uv = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
	o.alpha = i.alpha;
	o.tile = i.tile;
	return o;
}

float4 ps(VOut i) : SV_Target {
	float4 t = atlas.Sample(linearClamp, float3(i.uv, i.tile));
	if (i.tile == 1)
		return t * i.alpha; // the drop carries its own (premultiplied) colours
	return float4(tint.rgb, 1.0) * (t.a * i.alpha);
}
)";

typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* SerializeFn)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

// ---- The atlas, drawn once on the CPU ----

float Hash(int x, int y, int seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xffffff) / (float)0xffffff;
}

// Smooth value noise that wraps every period cells
float Noise(float x, float y, int period, int seed)
{
	int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
	float fx = x - x0, fy = y - y0;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	auto at = [&](int i, int j) { return Hash(((i % period) + period) % period, ((j % period) + period) % period, seed); };
	float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
	float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
	return a + (b - a) * fy;
}

float Smooth(float lo, float hi, float x)
{
	float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
	return t * t * (3 - 2 * t);
}

// Premultiplied RGBA, 0..1, for one tile at mip 0
void DrawTile(UINT tile, std::vector<float>& rgba)
{
	rgba.assign(kTileSize * kTileSize * 4, 0.0f);
	for (UINT y = 0; y < kTileSize; y++) {
		for (UINT x = 0; x < kTileSize; x++) {
			float u = (x + 0.5f) / kTileSize * 2 - 1, v = (y + 0.5f) / kTileSize * 2 - 1; // -1..1, v down
			float r = std::sqrt(u * u + v * v);
			float* p = &rgba[(y * kTileSize + x) * 4];
			if (tile == Rain::kTilePuff) {
				// A soft round cloud with lumpy insides
				float n = 0, amp = 0.5f, freq = 4;
				for (int o = 0; o < 4; o++, amp *= 0.5f, freq *= 2)
					n += amp * Noise((u + 1) * freq, (v + 1) * freq, (int)freq * 2, 7 + o);
				float fall = (std::max)(0.0f, 1 - r * r);
				float a = fall * fall * (0.45f + 0.9f * n);
				a = std::clamp(a, 0.0f, 1.0f);
				p[0] = p[1] = p[2] = p[3] = a;
			} else if (tile == Rain::kTileDrop) {
				// A water drop seen on a visor: a slightly dark lens with a darker rim, a highlight up left and a
				// faint bright crescent where light comes through at the bottom
				float inside = 1 - Smooth(0.86f, 0.94f, r);
				float rim = Smooth(0.55f, 0.88f, r) * inside;
				float body = 0.16f * inside + 0.32f * rim;
				float c = 0, a = body; // black, alpha body
				float hx = u + 0.32f, hy = v + 0.36f;
				float spot = (1 - Smooth(0.0f, 0.26f, std::sqrt(hx * hx * 1.6f + hy * hy))) * inside * 0.85f;
				c = spot + c * (1 - spot);
				a = spot + a * (1 - spot);
				float cx = u - 0.05f, cy = v - 0.45f;
				float crescent = (1 - Smooth(0.15f, 0.45f, std::sqrt(cx * cx * 0.5f + cy * cy * 2.5f))) * inside * 0.28f;
				c = crescent + c * (1 - crescent);
				a = crescent + a * (1 - crescent);
				p[0] = p[1] = p[2] = c;
				p[3] = a;
			} else {
				p[0] = p[1] = p[2] = p[3] = 1.0f;
			}
		}
	}
}

} // namespace

RainDraw::~RainDraw()
{
	Release();
}

void RainDraw::WaitIdle()
{
	if (fence && lastQueue && fenceCounter > 0 && fence->GetCompletedValue() < fenceCounter) {
		fence->SetEventOnCompletion(fenceCounter, fenceEvent);
		WaitForSingleObject(fenceEvent, 5000);
	}
}

void RainDraw::Release()
{
	WaitIdle();
	for (Slot& s : slots) {
		if (s.upload && s.mapped)
			s.upload->Unmap(0, nullptr);
		s = Slot();
	}
	pipeline.Reset();
	pipelineFormat = DXGI_FORMAT_UNKNOWN;
	rootSignature.Reset();
	vertexShader.Reset();
	pixelShader.Reset();
	atlas.Reset();
	srvHeap.Reset();
	rtvHeap.Reset();
	fence.Reset();
	fenceCounter = 0;
	if (fenceEvent) {
		CloseHandle(fenceEvent);
		fenceEvent = nullptr;
	}
	device = nullptr;
	lastQueue = nullptr;
	failed = false;
}

bool RainDraw::Setup(ID3D12Device* dev, ID3D12CommandQueue* queue)
{
	device = dev;
	auto fail = [this](const char* what, HRESULT hr) {
		RAIN_LOG("Rain: %s failed (0x%08lx), so no spray or visor drops", what, (unsigned long)hr);
		failed = true;
		return false;
	};

	// Shaders, compiled here: d3dcompiler_47.dll is part of Windows
	HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	CompileFn compile = compiler ? (CompileFn)GetProcAddress(compiler, "D3DCompile") : nullptr;
	if (!compile)
		return fail("loading d3dcompiler_47.dll", E_FAIL);
	ComPtr<ID3DBlob> errors;
	HRESULT hr = compile(kShaders, sizeof(kShaders) - 1, "rain", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertexShader, &errors);
	if (SUCCEEDED(hr))
		hr = compile(kShaders, sizeof(kShaders) - 1, "rain", nullptr, nullptr, "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixelShader, &errors);
	if (FAILED(hr)) {
		if (errors)
			RAIN_LOG("Rain: shader errors: %.*s", (int)errors->GetBufferSize(), (const char*)errors->GetBufferPointer());
		return fail("compiling the shaders", hr);
	}

	// Root signature: the pass constants, the atlas, one sampler
	HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
	SerializeFn serialize = d3d12 ? (SerializeFn)GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr;
	if (!serialize)
		return fail("finding D3D12SerializeRootSignature", E_FAIL);
	D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	D3D12_ROOT_PARAMETER params[2] = {};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor = { 0, 0 };
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable = { 1, &range };
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rsDesc{ 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
	ComPtr<ID3DBlob> rsBlob;
	if (FAILED(hr = serialize(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errors)))
		return fail("D3D12SerializeRootSignature", hr);
	if (FAILED(hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature))))
		return fail("CreateRootSignature", hr);

	// Descriptor heaps: the atlas (shader visible) and the target's RTV
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
	if (FAILED(hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap))))
		return fail("CreateDescriptorHeap", hr);
	heapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
	if (FAILED(hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap))))
		return fail("CreateDescriptorHeap", hr);

	// Frame slots
	if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		return fail("CreateFence", hr);
	fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
	D3D12_RESOURCE_DESC bufferDesc{};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = kUploadSize;
	bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for (Slot& s : slots) {
		if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))))
			return fail("CreateCommandAllocator", hr);
		if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.list))))
			return fail("CreateCommandList", hr);
		s.list->Close();
		if (FAILED(hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s.upload))))
			return fail("CreateCommittedResource", hr);
		D3D12_RANGE none{ 0, 0 };
		if (FAILED(hr = s.upload->Map(0, &none, (void**)&s.mapped)))
			return fail("Map", hr);
	}

	if (!UploadAtlas(queue))
		return false;
	RAIN_LOG("Rain: ready to draw spray and visor drops");
	return true;
}

bool RainDraw::UploadAtlas(ID3D12CommandQueue* queue)
{
	auto fail = [this](const char* what, HRESULT hr) {
		RAIN_LOG("Rain: %s failed (0x%08lx) making the atlas, so no spray or visor drops", what, (unsigned long)hr);
		failed = true;
		return false;
	};

	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = desc.Height = kTileSize;
	desc.DepthOrArraySize = kTiles;
	desc.MipLevels = kMips;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
	HRESULT hr = device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&atlas));
	if (FAILED(hr))
		return fail("CreateCommittedResource", hr);

	const UINT count = kTiles * kMips;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[kTiles * kMips];
	UINT rows[kTiles * kMips];
	UINT64 rowSizes[kTiles * kMips], total = 0;
	device->GetCopyableFootprints(&desc, 0, count, 0, footprints, rows, rowSizes, &total);

	D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
	D3D12_RESOURCE_DESC bufferDesc{};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = total;
	bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> upload;
	if (FAILED(hr = device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
		return fail("CreateCommittedResource", hr);
	unsigned char* mapped = nullptr;
	if (FAILED(hr = upload->Map(0, nullptr, (void**)&mapped)))
		return fail("Map", hr);

	// Each tile, then its mips by halving (premultiplied, so a plain average is right)
	for (UINT tile = 0; tile < kTiles; tile++) {
		std::vector<float> level;
		DrawTile(tile, level);
		UINT size = kTileSize;
		for (UINT mip = 0; mip < kMips; mip++) {
			const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = footprints[tile * kMips + mip];
			for (UINT y = 0; y < size; y++) {
				unsigned char* row = mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
				for (UINT x = 0; x < size * 4; x++)
					row[x] = (unsigned char)std::lround(std::clamp(level[(size_t)y * size * 4 + x], 0.0f, 1.0f) * 255.0f);
			}
			if (size > 1) {
				UINT half = size / 2;
				std::vector<float> next((size_t)half * half * 4);
				for (UINT y = 0; y < half; y++)
					for (UINT x = 0; x < half; x++)
						for (int c = 0; c < 4; c++)
							next[((size_t)y * half + x) * 4 + c] = 0.25f
							    * (level[((size_t)(2 * y) * size + 2 * x) * 4 + c] + level[((size_t)(2 * y) * size + 2 * x + 1) * 4 + c]
							        + level[((size_t)(2 * y + 1) * size + 2 * x) * 4 + c] + level[((size_t)(2 * y + 1) * size + 2 * x + 1) * 4 + c]);
				level.swap(next);
				size = half;
			}
		}
	}
	upload->Unmap(0, nullptr);

	Slot& s = slots[0];
	s.allocator->Reset();
	s.list->Reset(s.allocator.Get(), nullptr);
	for (UINT i = 0; i < count; i++) {
		D3D12_TEXTURE_COPY_LOCATION dst{ atlas.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		dst.SubresourceIndex = i;
		D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
		src.PlacedFootprint = footprints[i];
		s.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = atlas.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	s.list->ResourceBarrier(1, &barrier);
	s.list->Close();
	ID3D12CommandList* lists[] = { s.list.Get() };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.Get(), ++fenceCounter);
	lastQueue = queue;
	WaitIdle(); // once, at the start; the upload buffer goes after this
	s.fenceValue = fenceCounter;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = desc.Format;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2DArray.MipLevels = kMips;
	srv.Texture2DArray.ArraySize = kTiles;
	device->CreateShaderResourceView(atlas.Get(), &srv, srvHeap->GetCPUDescriptorHandleForHeapStart());
	return true;
}

bool RainDraw::MakePipeline(DXGI_FORMAT format)
{
	D3D12_INPUT_ELEMENT_DESC inputs[] = {
		{ "POS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Instance, pos), D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "HALF", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Instance, half), D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "ANGLE", 0, DXGI_FORMAT_R32_FLOAT, 0, offsetof(Instance, angle), D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "ALPHA", 0, DXGI_FORMAT_R32_FLOAT, 0, offsetof(Instance, alpha), D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
		{ "TILE", 0, DXGI_FORMAT_R32_UINT, 0, offsetof(Instance, tile), D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
	};
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = rootSignature.Get();
	desc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	desc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
	blend.BlendEnable = TRUE;
	blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE; // premultiplied "over"
	blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	desc.SampleMask = UINT_MAX;
	desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.RasterizerState.DepthClipEnable = TRUE;
	desc.InputLayout = { inputs, (UINT)(sizeof(inputs) / sizeof(inputs[0])) };
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = format;
	desc.SampleDesc.Count = 1;
	pipeline.Reset();
	HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
	if (FAILED(hr)) {
		RAIN_LOG("Rain: CreateGraphicsPipelineState failed (0x%08lx) for format %d, so no spray or visor drops", (unsigned long)hr, (int)format);
		failed = true;
		return false;
	}
	pipelineFormat = format;
	return true;
}

bool RainDraw::Render(ID3D12Device* dev, ID3D12CommandQueue* queue, ID3D12Resource* target, DXGI_FORMAT format,
    const Pass* passes, size_t passCount)
{
	if (device && dev != device)
		Release(); // a new device: start over
	if (failed)
		return false;
	if (!device && !Setup(dev, queue))
		return false;
	if (format != pipelineFormat && !MakePipeline(format))
		return false;
	lastQueue = queue;

	Slot& s = slots[nextSlot];
	nextSlot = (nextSlot + 1) % kSlots;
	if (fence->GetCompletedValue() < s.fenceValue) {
		fence->SetEventOnCompletion(s.fenceValue, fenceEvent);
		WaitForSingleObject(fenceEvent, 1000);
	}
	s.allocator->Reset();
	s.list->Reset(s.allocator.Get(), pipeline.Get());

	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = format;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	device->CreateRenderTargetView(target, &rtvDesc, rtv);
	const float clear[4] = { 0, 0, 0, 0 };
	s.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
	s.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	s.list->SetGraphicsRootSignature(rootSignature.Get());
	ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
	s.list->SetDescriptorHeaps(1, heaps);
	s.list->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());
	s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	// Constants first, then each distinct sprite list once (both eyes share the spray)
	UINT64 used = (UINT64)kConstantsSize * kMaxPasses;
	const std::vector<Rain::Sprite>* written[kMaxPasses] = {};
	UINT64 offsets[kMaxPasses] = {};
	D3D12_GPU_VIRTUAL_ADDRESS base = s.upload->GetGPUVirtualAddress();
	for (size_t p = 0; p < passCount && p < kMaxPasses; p++) {
		const Pass& pass = passes[p];
		Constants c{};
		memcpy(c.viewProj, pass.viewProj, sizeof(c.viewProj));
		memcpy(c.right, pass.right, sizeof(pass.right));
		memcpy(c.up, pass.up, sizeof(pass.up));
		memcpy(c.tint, kMistColour, sizeof(kMistColour));
		c.tint[3] = 1;
		memcpy(s.mapped + p * kConstantsSize, &c, sizeof(c));

		const std::vector<Rain::Sprite>& sprites = *pass.sprites;
		if (sprites.empty())
			continue;
		UINT64 offset = 0;
		bool found = false;
		for (size_t q = 0; q < p; q++)
			if (written[q] == pass.sprites) {
				offset = offsets[q];
				found = true;
			}
		size_t count = (std::min)(sprites.size(), (size_t)((kUploadSize - used) / sizeof(Instance)));
		if (!found) {
			offset = used;
			Instance* out = (Instance*)(s.mapped + offset);
			for (size_t i = 0; i < count; i++) {
				const Rain::Sprite& sp = sprites[i];
				out[i] = { { sp.pos.x, sp.pos.y, sp.pos.z }, { sp.halfWidth, sp.halfHeight }, sp.angle, sp.alpha, sp.tile };
			}
			used += count * sizeof(Instance);
		}
		written[p] = pass.sprites;
		offsets[p] = offset;

		D3D12_VIEWPORT viewport{ (float)pass.rect.left, (float)pass.rect.top, (float)(pass.rect.right - pass.rect.left),
			(float)(pass.rect.bottom - pass.rect.top), 0.0f, 1.0f };
		s.list->RSSetViewports(1, &viewport);
		s.list->RSSetScissorRects(1, &pass.rect);
		s.list->SetGraphicsRootConstantBufferView(0, base + p * kConstantsSize);
		D3D12_VERTEX_BUFFER_VIEW vb{ base + offset, (UINT)(count * sizeof(Instance)), sizeof(Instance) };
		s.list->IASetVertexBuffers(0, 1, &vb);
		s.list->DrawInstanced(4, (UINT)count, 0, 0);
	}

	s.list->Close();
	ID3D12CommandList* lists[] = { s.list.Get() };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.Get(), ++fenceCounter);
	s.fenceValue = fenceCounter;
	return true;
}

void RainDraw::EyeCamera(const float q[4], const float p[3], float angleLeft, float angleRight, float angleUp, float angleDown, Pass& pass)
{
	// Rotation from the quaternion (x, y, z, w); columns are the eye's right, up and back axes
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float r[3][3] = {
		{ 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
		{ 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
		{ 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) },
	};
	// View: the transpose rotation, and the position moved back
	float view[4][4] = {};
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			view[i][j] = r[j][i];
		view[i][3] = -(r[0][i] * p[0] + r[1][i] * p[1] + r[2][i] * p[2]);
	}
	view[3][3] = 1;

	// Projection for an asymmetric field of view, looking down -z, depth 0..1
	float l = std::tan(angleLeft), rt = std::tan(angleRight), u = std::tan(angleUp), d = std::tan(angleDown);
	const float n = 0.05f, f = 2000.0f;
	float proj[4][4] = {
		{ 2 / (rt - l), 0, (rt + l) / (rt - l), 0 },
		{ 0, 2 / (u - d), (u + d) / (u - d), 0 },
		{ 0, 0, f / (n - f), n * f / (n - f) },
		{ 0, 0, -1, 0 },
	};
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			float sum = 0;
			for (int k = 0; k < 4; k++)
				sum += proj[i][k] * view[k][j];
			pass.viewProj[i * 4 + j] = sum;
		}
	for (int i = 0; i < 3; i++) {
		pass.right[i] = r[i][0];
		pass.up[i] = r[i][1];
	}
}

void RainDraw::PlaneCamera(float halfSize, Pass& pass)
{
	memset(pass.viewProj, 0, sizeof(pass.viewProj));
	pass.viewProj[0] = 1 / halfSize;
	pass.viewProj[5] = 1 / halfSize;
	pass.viewProj[11] = 0.5f; // depth: the middle
	pass.viewProj[15] = 1;
	pass.right[0] = 1, pass.right[1] = 0, pass.right[2] = 0;
	pass.up[0] = 0, pass.up[1] = 1, pass.up[2] = 0;
}

#endif
