#ifdef _WIN32

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include "scrcpy-d3d11-cst.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mem.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(x) \
	do { \
		if ((x) != NULL) { \
			(x)->lpVtbl->Release(x); \
			(x) = NULL; \
		} \
	} while (0)
#endif

struct d3d11_cst_params {
	uint32_t srcTransfer;
	uint32_t targetTransfer;
	uint32_t srcPrimaries;
	uint32_t targetPrimaries;
	uint32_t srcSlice;
	uint32_t width;
	uint32_t height;
	uint32_t reserved0;
	float srcFullRange;
	float peak;
	float reserved1;
	float reserved2;
};

struct gpu_profile {
	uint32_t primaries;
	uint32_t transfer;
	bool hdr;
	bool output_8bit;
	enum AVColorPrimaries av_primaries;
	enum AVColorSpace av_space;
	enum AVColorTransferCharacteristic av_trc;
};

struct scrcpy_d3d11_cst {
	AVBufferRef *hw_device_ref;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	ID3D11ComputeShader *shader_8bit;
	ID3D11ComputeShader *shader_10bit;
	ID3D11Buffer *params_buffer;

	ID3D11Texture2D *input_texture;
	ID3D11ShaderResourceView *input_y_srv;
	ID3D11ShaderResourceView *input_uv_srv;

	ID3D11Texture2D *output_y_texture;
	ID3D11Texture2D *output_uv_texture;
	ID3D11UnorderedAccessView *output_y_uav;
	ID3D11UnorderedAccessView *output_uv_uav;
	ID3D11Texture2D *staging_y_texture;
	ID3D11Texture2D *staging_uv_texture;

	int source_profile;
	int target_profile;
	int width;
	int height;
	bool logical_target_8bit;
	bool output_10bit;
	bool disabled;
};

static const char d3d11_cst_shader[] = "cbuffer CstParams : register(b0)\n{\n    uint srcTransfer;\n    uint targetTransfer;\n    uint srcPrimaries;\n    uint targetPrimaries;\n    uint srcSlice;\n    uint width;\n    uint height;\n    uint reserved0;\n    float srcFullRange;\n    float peak;\n    float reserved1;\n    float reserved2;\n};\n\nTexture2DArray<float> SrcY : register(t0);\nTexture2DArray<float2> SrcUV : register(t1);\n\n#if OUTPUT_10BIT\nRWTexture2D<uint> DstY : register(u0);\nRWTexture2D<uint> DstUV : register(u1);\n#else\nRWTexture2D<uint> DstY : register(u0);\nRWTexture2D<uint2> DstUV : register(u1);\n#endif\n\nstatic const float3x3 M709To2020 = float3x3(\n    0.6274039, 0.3292830, 0.0433131,\n    0.0690973, 0.9195404, 0.0113623,\n    0.0163914, 0.0880133, 0.8955953);\n\nstatic const float3x3 M2020To709 = float3x3(\n    1.6604910, -0.58764114, -0.07284986,\n    -0.12455047, 1.1328999, -0.00834942,\n    -0.01815076, -0.1005789, 1.11872966);\n\nfloat decodeTransfer(float x, uint id)\n{\n    x = max(x, 0.0);\n\n    if (id == 1)\n        return pow(x, 2.2);\n    if (id == 2)\n        return pow(x, 2.4);\n    if (id == 3)\n        return x <= 0.081 ? x / 4.5 : pow((x + 0.099) / 1.099, 1.0 / 0.45);\n    if (id == 4)\n        return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);\n    if (id == 5)\n        return x <= 0.5 ? (x * x) / 3.0 :\n               (exp((x - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;\n    if (id == 6) {\n        const float p = pow(x, 1.0 / 78.84375);\n        return pow(max(p - 0.8359375, 0.0) /\n                   max(18.8515625 - 18.6875 * p, 1e-6),\n                   1.0 / 0.1593017578);\n    }\n\n    return x;\n}\n\nfloat encodeTransfer(float x, uint id)\n{\n    x = max(x, 0.0);\n\n    if (id == 1)\n        return pow(x, 1.0 / 2.2);\n    if (id == 2)\n        return pow(x, 1.0 / 2.4);\n    if (id == 3)\n        return x <= 0.018 ? 4.5 * x : 1.099 * pow(x, 0.45) - 0.099;\n    if (id == 4)\n        return x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055;\n    if (id == 5)\n        return x <= (1.0 / 12.0) ? sqrt(3.0 * x) :\n               0.17883277 * log(12.0 * x - 0.28466892) + 0.55991073;\n    if (id == 6) {\n        const float p = pow(x, 0.1593017578);\n        return pow((0.8359375 + 18.8515625 * p) /\n                   (1.0 + 18.6875 * p),\n                   78.84375);\n    }\n\n    return x;\n}\n\nfloat3 yuvToRgb(float y, float2 uv)\n{\n    const bool full = srcFullRange > 0.5;\n    if (!full) {\n        y = (y - (64.0 / 1023.0)) / (876.0 / 1023.0);\n        uv = (uv - (512.0 / 1023.0)) / (896.0 / 1023.0);\n    } else {\n        uv = uv - (512.0 / 1023.0);\n    }\n\n    const float kb = srcPrimaries == 1 ? 0.0593 : 0.0722;\n    const float kr = srcPrimaries == 1 ? 0.2627 : 0.2126;\n    const float kg = 1.0 - kr - kb;\n\n    return float3(\n        y + 2.0 * (1.0 - kr) * uv.y,\n        y - 2.0 * kb * (1.0 - kb) / kg * uv.x\n          - 2.0 * kr * (1.0 - kr) / kg * uv.y,\n        y + 2.0 * (1.0 - kb) * uv.x);\n}\n\nfloat3 toneMapMobius(float3 rgb)\n{\n    const float3 lumaCoeffs = srcPrimaries == 1 ?\n        float3(0.2627, 0.6780, 0.0593) :\n        float3(0.2126, 0.7152, 0.0722);\n\n    const float luma = dot(lumaCoeffs, rgb);\n    if (luma > 2.0)\n        rgb = lerp(rgb, luma.xxx, saturate((luma - 2.0) / max(luma, 1e-6)));\n\n    const float sig = max(max(rgb.r, rgb.g), max(rgb.b, 1e-6));\n    const float j = 0.3;\n    const float p = max(peak, 1e-6);\n    const float a = -j * j * (p - 1.0) / (j * j - 2.0 * j + p);\n    const float b = (j * j - 2.0 * j * p + p) / max(p - 1.0, 1e-6);\n    float mapped = sig;\n\n    if (sig > j)\n        mapped = (b * b + 2.0 * b * j + j * j) / max(b - a, 1e-6) * (sig + a) / max(sig + b, 1e-6);\n\n    return rgb * (mapped / sig);\n}\n\nfloat3 transformRgb(float3 rgb)\n{\n    if (srcTransfer != 9)\n        rgb = float3(\n            decodeTransfer(rgb.r, srcTransfer),\n            decodeTransfer(rgb.g, srcTransfer),\n            decodeTransfer(rgb.b, srcTransfer));\n\n    if (srcPrimaries != targetPrimaries) {\n        if (srcPrimaries == 0 && targetPrimaries == 1)\n            rgb = mul(M709To2020, rgb);\n        else if (srcPrimaries == 1 && targetPrimaries == 0)\n            rgb = mul(M2020To709, rgb);\n    }\n\n    if ((srcTransfer == 5 || srcTransfer == 6) && targetTransfer != 5 && targetTransfer != 6)\n        rgb = toneMapMobius(rgb);\n\n    if (targetTransfer != 9)\n        rgb = float3(\n            encodeTransfer(rgb.r, targetTransfer),\n            encodeTransfer(rgb.g, targetTransfer),\n            encodeTransfer(rgb.b, targetTransfer));\n\n    return rgb;\n}\n\nfloat3 rgbToYuv(float3 rgb)\n{\n    const float kb = targetPrimaries == 1 ? 0.0593 : 0.0722;\n    const float kr = targetPrimaries == 1 ? 0.2627 : 0.2126;\n    const float kg = 1.0 - kr - kb;\n    const float y = kr * rgb.r + kg * rgb.g + kb * rgb.b;\n    const float u = (rgb.b - y) / (2.0 * (1.0 - kb)) + 0.5;\n    const float v = (rgb.r - y) / (2.0 * (1.0 - kr)) + 0.5;\n    return float3(y, u, v);\n}\n\nfloat3 sampleRgb(uint2 p, float2 uv)\n{\n    const float y = SrcY.Load(int4(p.x, p.y, srcSlice, 0));\n    return transformRgb(yuvToRgb(y, uv));\n}\n\nvoid writeLuma(uint2 p, float value)\n{\n#if OUTPUT_10BIT\n    DstY[p] = (uint)round(saturate(value) * 1023.0) << 6;\n#else\n    DstY[p] = (uint)round(saturate(value) * 255.0);\n#endif\n}\n\nvoid writeChroma(uint2 p, float2 uv)\n{\n#if OUTPUT_10BIT\n    const uint u = (uint)round(saturate(uv.x) * 1023.0) << 6;\n    const uint v = (uint)round(saturate(uv.y) * 1023.0) << 6;\n    DstUV[p] = u | (v << 16);\n#else\n    const uint u = (uint)round(saturate(uv.x) * 255.0);\n    const uint v = (uint)round(saturate(uv.y) * 255.0);\n    DstUV[p] = uint2(u, v);\n#endif\n}\n\n[numthreads(8, 8, 1)]\nvoid main(uint3 id : SV_DispatchThreadID)\n{\n    const uint2 chromaPos = id.xy;\n    const uint2 base = chromaPos * 2;\n\n    if (base.x >= width || base.y >= height)\n        return;\n\n    const uint2 maxPos = uint2(width - 1, height - 1);\n    const uint2 p0 = base;\n    const uint2 p1 = min(base + uint2(1, 0), maxPos);\n    const uint2 p2 = min(base + uint2(0, 1), maxPos);\n    const uint2 p3 = min(base + uint2(1, 1), maxPos);\n\n    const float2 uv = SrcUV.Load(int4(chromaPos.x, chromaPos.y, srcSlice, 0));\n\n    const float3 rgb0 = sampleRgb(p0, uv);\n    const float3 rgb1 = sampleRgb(p1, uv);\n    const float3 rgb2 = sampleRgb(p2, uv);\n    const float3 rgb3 = sampleRgb(p3, uv);\n\n    const float3 yuv0 = rgbToYuv(rgb0);\n    const float3 yuv1 = rgbToYuv(rgb1);\n    const float3 yuv2 = rgbToYuv(rgb2);\n    const float3 yuv3 = rgbToYuv(rgb3);\n\n    writeLuma(p0, yuv0.x);\n    writeLuma(p1, yuv1.x);\n    writeLuma(p2, yuv2.x);\n    writeLuma(p3, yuv3.x);\n\n    writeChroma(chromaPos, (yuv0.yz + yuv1.yz + yuv2.yz + yuv3.yz) * 0.25);\n}\n";

static void log_hresult(const char *op, HRESULT hr)
{
	obs_log(LOG_WARNING, "scrcpy-d3d11-cst: %s failed (HRESULT=0x%08lx)", op, (unsigned long)hr);
}

static bool profile_from_frame(int profile, const AVFrame *frame, struct gpu_profile *p)
{
	int encoded = profile;
	int color_space;
	int gamma;

	if (!p)
		return false;

	memset(p, 0, sizeof(*p));
	p->primaries = 0;
	p->transfer = 3;
	p->av_primaries = AVCOL_PRI_BT709;
	p->av_space = AVCOL_SPC_BT709;
	p->av_trc = AVCOL_TRC_BT709;

	if (profile > 0) {
		if (profile >= 1000 && profile < 1100) {
			encoded = profile - 1000;
			p->output_8bit = true;
		}

		color_space = encoded / 10;
		gamma = encoded % 10;
		if (color_space < 1 || color_space > 3 || gamma < 1 || gamma > 9)
			return false;

		p->primaries = color_space == 3 ? 1U : 0U;
		p->av_primaries = p->primaries ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
		p->av_space = p->primaries ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;

		switch (gamma) {
		case 1:
			p->transfer = 1;
			p->av_trc = AVCOL_TRC_GAMMA22;
			break;
		case 2:
			p->transfer = 2;
			p->av_trc = AVCOL_TRC_SMPTE170M;
			break;
		case 3:
			p->transfer = 3;
			p->av_trc = AVCOL_TRC_BT709;
			break;
		case 4:
		case 8:
			p->transfer = 4;
			p->av_trc = AVCOL_TRC_IEC61966_2_1;
			break;
		case 5:
			p->transfer = 5;
			p->av_trc = AVCOL_TRC_ARIB_STD_B67;
			break;
		case 6:
		case 7:
			p->transfer = 6;
			p->av_trc = AVCOL_TRC_SMPTE2084;
			break;
		case 9:
			p->transfer = 9;
			p->av_trc = AVCOL_TRC_LINEAR;
			break;
		default:
			return false;
		}

		p->hdr = p->transfer == 5 || p->transfer == 6;
		return p->transfer != 9;
	}

	if (!frame)
		return false;

	if (frame->color_primaries == AVCOL_PRI_BT2020 || frame->colorspace == AVCOL_SPC_BT2020_NCL ||
	    frame->colorspace == AVCOL_SPC_BT2020_CL) {
		p->primaries = 1;
		p->av_primaries = AVCOL_PRI_BT2020;
		p->av_space = frame->colorspace == AVCOL_SPC_BT2020_CL ? AVCOL_SPC_BT2020_CL : AVCOL_SPC_BT2020_NCL;
	}

	switch (frame->color_trc) {
	case AVCOL_TRC_GAMMA22:
		p->transfer = 1;
		p->av_trc = AVCOL_TRC_GAMMA22;
		break;
	case AVCOL_TRC_SMPTE170M:
		p->transfer = 2;
		p->av_trc = AVCOL_TRC_SMPTE170M;
		break;
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_UNSPECIFIED:
		p->transfer = 3;
		p->av_trc = AVCOL_TRC_BT709;
		break;
	case AVCOL_TRC_IEC61966_2_1:
		p->transfer = 4;
		p->av_trc = AVCOL_TRC_IEC61966_2_1;
		break;
	case AVCOL_TRC_ARIB_STD_B67:
		p->transfer = 5;
		p->av_trc = AVCOL_TRC_ARIB_STD_B67;
		break;
	case AVCOL_TRC_SMPTE2084:
		p->transfer = 6;
		p->av_trc = AVCOL_TRC_SMPTE2084;
		break;
	case AVCOL_TRC_LINEAR:
		return false;
	case AVCOL_TRC_BT2020_10:
	case AVCOL_TRC_BT2020_12:
		p->transfer = 3;
		p->av_trc = AVCOL_TRC_BT709;
		break;
	default:
		p->transfer = 3;
		p->av_trc = AVCOL_TRC_BT709;
		break;
	}

	p->hdr = p->transfer == 5 || p->transfer == 6;
	return true;
}

static void release_input_views(struct scrcpy_d3d11_cst *cst)
{
	SAFE_RELEASE(cst->input_y_srv);
	SAFE_RELEASE(cst->input_uv_srv);
	SAFE_RELEASE(cst->input_texture);
}

static void release_output_resources(struct scrcpy_d3d11_cst *cst)
{
	SAFE_RELEASE(cst->output_y_uav);
	SAFE_RELEASE(cst->output_uv_uav);
	SAFE_RELEASE(cst->output_y_texture);
	SAFE_RELEASE(cst->output_uv_texture);
	SAFE_RELEASE(cst->staging_y_texture);
	SAFE_RELEASE(cst->staging_uv_texture);
	cst->width = 0;
	cst->height = 0;
}

static bool create_2d_texture(struct scrcpy_d3d11_cst *cst, UINT width, UINT height, DXGI_FORMAT format,
			      UINT bind_flags, D3D11_USAGE usage, UINT cpu_access, ID3D11Texture2D **texture)
{
	D3D11_TEXTURE2D_DESC desc = {0};
	HRESULT hr;

	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = usage;
	desc.BindFlags = bind_flags;
	desc.CPUAccessFlags = cpu_access;

	hr = ID3D11Device_CreateTexture2D(cst->device, &desc, NULL, texture);
	if (FAILED(hr)) {
		log_hresult("CreateTexture2D", hr);
		return false;
	}
	return true;
}

static bool create_output_resources(struct scrcpy_d3d11_cst *cst, bool output_10bit)
{
	const DXGI_FORMAT y_format = output_10bit ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R8_UINT;
	const DXGI_FORMAT uv_format = output_10bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8G8_UINT;
	HRESULT hr;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {0};

	release_output_resources(cst);

	if (cst->width <= 0 || cst->height <= 0 || (cst->width & 1) || (cst->height & 1))
		return false;

	if (!create_2d_texture(cst, (UINT)cst->width, (UINT)cst->height, y_format, D3D11_BIND_UNORDERED_ACCESS,
			       D3D11_USAGE_DEFAULT, 0, &cst->output_y_texture))
		goto fail;
	if (!create_2d_texture(cst, (UINT)(cst->width / 2), (UINT)(cst->height / 2), uv_format,
			       D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, &cst->output_uv_texture))
		goto fail;

	uav.Format = y_format;
	uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	uav.Texture2D.MipSlice = 0;
	hr = ID3D11Device_CreateUnorderedAccessView(cst->device, (ID3D11Resource *)cst->output_y_texture, &uav,
	{6} {4}&cst->output_y_uav);
	if (FAILED(hr)) {
		log_hresult("CreateUnorderedAccessView(Y)", hr);
		goto fail;
	}

	uav.Format = uv_format;
	hr = ID3D11Device_CreateUnorderedAccessView(cst->device, (ID3D11Resource *)cst->output_uv_texture, &uav,
	{6} {4}&cst->output_uv_uav);
	if (FAILED(hr)) {
		log_hresult("CreateUnorderedAccessView(UV)", hr);
		goto fail;
	}

	if (!create_2d_texture(cst, (UINT)cst->width, (UINT)cst->height, y_format, 0, D3D11_USAGE_STAGING,
			       D3D11_CPU_ACCESS_READ, &cst->staging_y_texture))
		goto fail;
	if (!create_2d_texture(cst, (UINT)(cst->width / 2), (UINT)(cst->height / 2), uv_format, 0, D3D11_USAGE_STAGING,
			       D3D11_CPU_ACCESS_READ, &cst->staging_uv_texture))
		goto fail;

	cst->output_10bit = output_10bit;
	return true;

fail:
	release_output_resources(cst);
	return false;
}

static bool create_input_views(struct scrcpy_d3d11_cst *cst, ID3D11Texture2D *texture, UINT array_size)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC srv = {0};
	HRESULT hr;

	release_input_views(cst);
	ID3D11Texture2D_AddRef(texture);
	cst->input_texture = texture;

	srv.Format = DXGI_FORMAT_R16_UNORM;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srv.Texture2DArray.MostDetailedMip = 0;
	srv.Texture2DArray.MipLevels = 1;
	srv.Texture2DArray.FirstArraySlice = 0;
	srv.Texture2DArray.ArraySize = array_size;
	hr = ID3D11Device_CreateShaderResourceView(cst->device, (ID3D11Resource *)texture, &srv, &cst->input_y_srv);
	if (FAILED(hr)) {
		log_hresult("CreateShaderResourceView(Y)", hr);
		release_input_views(cst);
		return false;
	}

	srv.Format = DXGI_FORMAT_R16G16_UNORM;
	hr = ID3D11Device_CreateShaderResourceView(cst->device, (ID3D11Resource *)texture, &srv, &cst->input_uv_srv);
	if (FAILED(hr)) {
		log_hresult("CreateShaderResourceView(UV)", hr);
		release_input_views(cst);
		return false;
	}

	return true;
}

static bool ensure_resources(struct scrcpy_d3d11_cst *cst, const AVFrame *input)
{
	ID3D11Texture2D *texture;
	D3D11_TEXTURE2D_DESC desc = {0};
	UINT array_size;
	UINT slice;
	bool output_10bit;

	if (!input || input->format != AV_PIX_FMT_D3D11 || !input->data[0] || !input->data[1])
		return false;

	texture = (ID3D11Texture2D *)input->data[0];
	ID3D11Texture2D_GetDesc(texture, &desc);
	if (desc.Format != DXGI_FORMAT_P010 || desc.ArraySize == 0) {
		obs_log(LOG_WARNING, "scrcpy-d3d11-cst: unsupported decoder texture format=%u array=%u",
			(unsigned)desc.Format, (unsigned)desc.ArraySize);
		return false;
	}

	array_size = desc.ArraySize;
	slice = (UINT)(uintptr_t)input->data[1];
	if (slice >= array_size) {
		obs_log(LOG_WARNING, "scrcpy-d3d11-cst: invalid decoder surface index %u/%u", (unsigned)slice,
			(unsigned)array_size);
		return false;
	}

	output_10bit = !cst->logical_target_8bit;
	if (cst->output_y_texture &&
	    (cst->width != input->width || cst->height != input->height || cst->output_10bit != output_10bit))
		release_output_resources(cst);

	cst->width = input->width;
	cst->height = input->height;

	if (!cst->output_y_texture && !create_output_resources(cst, output_10bit))
		return false;

	if (cst->input_texture != texture || !cst->input_y_srv || !cst->input_uv_srv) {
		if (!create_input_views(cst, texture, array_size))
			return false;
	} else {
		D3D11_TEXTURE2D_DESC cached_desc = {0};
		ID3D11Texture2D_GetDesc(cst->input_texture, &cached_desc);
		if (cached_desc.ArraySize != desc.ArraySize || cached_desc.Format != desc.Format) {
			if (!create_input_views(cst, texture, array_size))
				return false;
		}
	}

	return true;
}

static bool update_params(struct scrcpy_d3d11_cst *cst, const struct gpu_profile *source,
			  const struct gpu_profile *target, const AVFrame *input, enum AVColorRange input_range)
{
	D3D11_MAPPED_SUBRESOURCE mapped = {0};
	struct d3d11_cst_params *params;
	HRESULT hr = ID3D11DeviceContext_Map(cst->context, (ID3D11Resource *)cst->params_buffer, 0,
					     D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	if (FAILED(hr)) {
		log_hresult("Map(constant buffer)", hr);
		return false;
	}

	params = mapped.pData;
	memset(params, 0, sizeof(*params));
	params->srcTransfer = source->transfer;
	params->targetTransfer = target->transfer;
	params->srcPrimaries = source->primaries;
	params->targetPrimaries = target->primaries;
	params->srcSlice = (UINT)(uintptr_t)input->data[1];
	params->width = (UINT)input->width;
	params->height = (UINT)input->height;
	params->srcFullRange = input_range == AVCOL_RANGE_JPEG ? 1.0f : 0.0f;
	params->peak = 1.0f;
	ID3D11DeviceContext_Unmap(cst->context, (ID3D11Resource *)cst->params_buffer, 0);
	return true;
}

static bool create_params_buffer(struct scrcpy_d3d11_cst *cst)
{
	D3D11_BUFFER_DESC desc = {0};
	HRESULT hr;

	desc.ByteWidth = sizeof(struct d3d11_cst_params);
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	hr = ID3D11Device_CreateBuffer(cst->device, &desc, NULL, &cst->params_buffer);
	if (FAILED(hr)) {
		log_hresult("CreateBuffer", hr);
		return false;
	}
	return true;
}

static bool compile_shader(ID3D11Device *device, const char *source_code, bool output_10bit,
			   ID3D11ComputeShader **shader)
{
	const D3D_SHADER_MACRO defines[] = {
		{"OUTPUT_10BIT", output_10bit ? "1" : "0"},
		{NULL, NULL},
	};
	ID3DBlob *code = NULL;
	ID3DBlob *errors = NULL;
	HRESULT hr;

	hr = D3DCompile(source_code, strlen(source_code), "droidsource_cst.hlsl", defines, NULL, "main", "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr)) {
		if (errors) {
			obs_log(LOG_WARNING, "scrcpy-d3d11-cst: shader compilation failed: %.*s",
				(int)ID3D10Blob_GetBufferSize(errors),
				(const char *)ID3D10Blob_GetBufferPointer(errors));
		} else {
			log_hresult("D3DCompile", hr);
		}
		SAFE_RELEASE(errors);
		SAFE_RELEASE(code);
		return false;
	}

	hr = ID3D11Device_CreateComputeShader(device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
					      NULL, shader);
	SAFE_RELEASE(errors);
	SAFE_RELEASE(code);
	if (FAILED(hr)) {
		log_hresult("CreateComputeShader", hr);
		return false;
	}
	return true;
}

static bool copy_plane_to_frame(struct scrcpy_d3d11_cst *cst, ID3D11Texture2D *staging, uint8_t *dst, int dst_linesize,
				int height, size_t row_bytes)
{
	D3D11_MAPPED_SUBRESOURCE mapped = {0};
	HRESULT hr = ID3D11DeviceContext_Map(cst->context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		log_hresult("Map(staging)", hr);
		return false;
	}

	for (int y = 0; y < height; ++y)
		memcpy(dst + (size_t)y * (size_t)dst_linesize,
		       (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch, row_bytes);

	ID3D11DeviceContext_Unmap(cst->context, (ID3D11Resource *)staging, 0);
	return true;
}

static bool copy_staging_to_frame(struct scrcpy_d3d11_cst *cst, const AVFrame *input, AVFrame *output)
{
	const enum AVPixelFormat output_format = cst->output_10bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
	const size_t row_bytes = (size_t)input->width * (cst->output_10bit ? 2U : 1U);

	av_frame_unref(output);
	output->format = output_format;
	output->width = input->width;
	output->height = input->height;
	if (av_frame_copy_props(output, input) < 0)
		return false;
	if (av_frame_get_buffer(output, 32) < 0)
		return false;

	if (!copy_plane_to_frame(cst, cst->staging_y_texture, output->data[0], output->linesize[0], input->height,
				 row_bytes))
		return false;
	return copy_plane_to_frame(cst, cst->staging_uv_texture, output->data[1], output->linesize[1],
				   input->height / 2, row_bytes);
}

scrcpy_d3d11_cst_t *scrcpy_d3d11_cst_create(AVBufferRef *hw_device_ctx, int source_profile, int target_profile)
{
	struct scrcpy_d3d11_cst *cst;
	struct gpu_profile target;
	AVHWDeviceContext *av_device;
	AVD3D11VADeviceContext *d3d;

	if (!hw_device_ctx || target_profile == 0)
		return NULL;

	av_device = (AVHWDeviceContext *)hw_device_ctx->data;
	if (!av_device || av_device->type != AV_HWDEVICE_TYPE_D3D11VA || !av_device->hwctx)
		return NULL;

	d3d = (AVD3D11VADeviceContext *)av_device->hwctx;
	if (!d3d->device || !d3d->device_context)
		return NULL;
	if (!profile_from_frame(target_profile, NULL, &target))
		return NULL;

	cst = bzalloc(sizeof(*cst));
	if (!cst)
		return NULL;

	cst->hw_device_ref = av_buffer_ref(hw_device_ctx);
	cst->device = d3d->device;
	cst->context = d3d->device_context;
	cst->source_profile = source_profile;
	cst->target_profile = target_profile;
	cst->logical_target_8bit = target.output_8bit || !target.hdr;
	cst->disabled = false;

	if (!cst->hw_device_ref ||
	    !compile_shader(cst->device, d3d11_cst_shader, false, &cst->shader_8bit) ||
	    !compile_shader(cst->device, d3d11_cst_shader, true, &cst->shader_10bit) ||{
		obs_log(LOG_WARNING, "scrcpy-d3d11-cst: initialization failed; using CPU CST fallback");
		scrcpy_d3d11_cst_destroy(cst);
		return NULL;
	}

	obs_log(LOG_INFO, "scrcpy-d3d11-cst: GPU CST initialized");
	return cst;
}

bool scrcpy_d3d11_cst_apply(scrcpy_d3d11_cst_t *cst, const AVFrame *input, AVFrame *output,
			    enum AVColorRange input_range)
{
	struct gpu_profile source;
	struct gpu_profile target;
	ID3D11UnorderedAccessView *uavs[2];
	ID3D11ShaderResourceView *srvs[2];
	ID3D11UnorderedAccessView *null_uavs[2] = {NULL, NULL};
	ID3D11ShaderResourceView *null_srvs[2] = {NULL, NULL};
	ID3D11Buffer *null_buffer = NULL;
	ID3D11Buffer *cbuffers[1];
	ID3D11ComputeShader *shader;

	if (!cst || cst->disabled || !input || !output)
		return false;

	if (!profile_from_frame(cst->source_profile, input, &source) ||
	    !profile_from_frame(cst->target_profile, NULL, &target))
		goto fail;

	if (!ensure_resources(cst, input))
		goto fail;

	if (!update_params(cst, &source, &target, input, input_range))
		goto fail;

	srvs[0] = cst->input_y_srv;
	srvs[1] = cst->input_uv_srv;
	uavs[0] = cst->output_y_uav;
	uavs[1] = cst->output_uv_uav;
	cbuffers[0] = cst->params_buffer;
	shader = cst->output_10bit ? cst->shader_10bit : cst->shader_8bit;

	ID3D11DeviceContext_CSSetShader(cst->context, shader, NULL, 0);
	ID3D11DeviceContext_CSSetConstantBuffers(cst->context, 0, 1, cbuffers);
	ID3D11DeviceContext_CSSetShaderResources(cst->context, 0, 2, srvs);
	ID3D11DeviceContext_CSSetUnorderedAccessViews(cst->context, 0, 2, uavs, NULL);

	ID3D11DeviceContext_Dispatch(cst->context, ((UINT)cst->width + 15U) / 16U, ((UINT)cst->height + 15U) / 16U, 1);

	ID3D11DeviceContext_CSSetShaderResources(cst->context, 0, 2, null_srvs);
	ID3D11DeviceContext_CSSetUnorderedAccessViews(cst->context, 0, 2, null_uavs, NULL);
	ID3D11DeviceContext_CSSetConstantBuffers(cst->context, 0, 1, &null_buffer);
	ID3D11DeviceContext_CSSetShader(cst->context, NULL, NULL, 0);

	ID3D11DeviceContext_CopyResource(cst->context, (ID3D11Resource *)cst->staging_y_texture,
					 (ID3D11Resource *)cst->output_y_texture);
	ID3D11DeviceContext_CopyResource(cst->context, (ID3D11Resource *)cst->staging_uv_texture,
					 (ID3D11Resource *)cst->output_uv_texture);

	if (!copy_staging_to_frame(cst, input, output))
		goto fail;

	output->color_primaries = target.av_primaries;
	output->colorspace = target.av_space;
	output->color_trc = target.av_trc;
	output->color_range = AVCOL_RANGE_JPEG;
	return true;

fail:
	if (cst) {
		cst->disabled = true;
		obs_log(LOG_WARNING, "scrcpy-d3d11-cst: GPU path disabled; reverting to CPU CST");
	}
	return false;
}

void scrcpy_d3d11_cst_destroy(scrcpy_d3d11_cst_t *cst)
{
	if (!cst)
		return;

	SAFE_RELEASE(cst->shader_8bit);
	SAFE_RELEASE(cst->shader_10bit);
	SAFE_RELEASE(cst->params_buffer);
	release_input_views(cst);
	release_output_resources(cst);
	if (cst->hw_device_ref)
		av_buffer_unref(&cst->hw_device_ref);
	bfree(cst);
}

#endif
