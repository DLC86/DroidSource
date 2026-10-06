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
#include <plugin-support.h>
#include <util/bmem.h>

#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mem.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(x) \
	do { \
		if ((x) != NULL) { \
			(x)->lpVtbl->Release(x); \
			(x) = NULL; \
		} \
	} while (0)
#endif

#define D3D11_CST_LUT_SIZE 1025
#define D3D11_CST_LUT_VECTORS 257

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
	float srcTransferLut[D3D11_CST_LUT_VECTORS][4];
	float targetTransferLut[D3D11_CST_LUT_VECTORS][4];
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

#define D3D11_CST_READBACK_SLOTS 3

struct d3d11_cst_readback_slot {
	ID3D11Texture2D *output_y_texture;
	ID3D11Texture2D *output_uv_texture;
	ID3D11UnorderedAccessView *output_y_uav;
	ID3D11UnorderedAccessView *output_uv_uav;
	ID3D11Texture2D *staging_y_texture;
	ID3D11Texture2D *staging_uv_texture;
	int64_t pts;
	bool pending;
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

	struct d3d11_cst_readback_slot slots[D3D11_CST_READBACK_SLOTS];

	int source_profile;
	int target_profile;
	int width;
	int height;
	bool logical_target_8bit;
	bool output_10bit;
	bool disabled;
	float src_transfer_lut[D3D11_CST_LUT_SIZE];
	float target_transfer_lut[D3D11_CST_LUT_SIZE];
	uint32_t cached_src_transfer;
	uint32_t cached_target_transfer;
	bool transfer_lut_valid;
};

static const char d3d11_cst_shader[] =
#include "scrcpy-d3d11-cst-shader.inc"
	;

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
	for (int i = 0; i < D3D11_CST_READBACK_SLOTS; ++i) {
		struct d3d11_cst_readback_slot *slot = &cst->slots[i];
		SAFE_RELEASE(slot->output_y_uav);
		SAFE_RELEASE(slot->output_uv_uav);
		SAFE_RELEASE(slot->output_y_texture);
		SAFE_RELEASE(slot->output_uv_texture);
		SAFE_RELEASE(slot->staging_y_texture);
		SAFE_RELEASE(slot->staging_uv_texture);
		slot->pts = AV_NOPTS_VALUE;
		slot->pending = false;
	}
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

	release_output_resources(cst);

	if (cst->width <= 0 || cst->height <= 0 || (cst->width & 1) || (cst->height & 1))
		return false;

	for (int i = 0; i < D3D11_CST_READBACK_SLOTS; ++i) {
		struct d3d11_cst_readback_slot *slot = &cst->slots[i];
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {0};

		if (!create_2d_texture(cst, (UINT)cst->width, (UINT)cst->height, y_format, D3D11_BIND_UNORDERED_ACCESS,
				       D3D11_USAGE_DEFAULT, 0, &slot->output_y_texture))
			goto fail;
		if (!create_2d_texture(cst, (UINT)(cst->width / 2), (UINT)(cst->height / 2), uv_format,
				       D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, &slot->output_uv_texture))
			goto fail;

		uav.Format = y_format;
		uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uav.Texture2D.MipSlice = 0;
		if (FAILED(ID3D11Device_CreateUnorderedAccessView(cst->device, (ID3D11Resource *)slot->output_y_texture,
								  &uav, &slot->output_y_uav))) {
			log_hresult("CreateUnorderedAccessView(Y)", E_FAIL);
			goto fail;
		}

		uav.Format = uv_format;
		if (FAILED(ID3D11Device_CreateUnorderedAccessView(
			    cst->device, (ID3D11Resource *)slot->output_uv_texture, &uav, &slot->output_uv_uav))) {
			log_hresult("CreateUnorderedAccessView(UV)", E_FAIL);
			goto fail;
		}

		if (!create_2d_texture(cst, (UINT)cst->width, (UINT)cst->height, y_format, 0, D3D11_USAGE_STAGING,
				       D3D11_CPU_ACCESS_READ, &slot->staging_y_texture))
			goto fail;
		if (!create_2d_texture(cst, (UINT)(cst->width / 2), (UINT)(cst->height / 2), uv_format, 0,
				       D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &slot->staging_uv_texture))
			goto fail;

		slot->pts = AV_NOPTS_VALUE;
		slot->pending = false;
	}

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
	if (cst->slots[0].output_y_texture &&
	    (cst->width != input->width || cst->height != input->height || cst->output_10bit != output_10bit))
		release_output_resources(cst);

	cst->width = input->width;
	cst->height = input->height;

	if (!cst->slots[0].output_y_texture && !create_output_resources(cst, output_10bit))
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


static float decode_transfer_cpu(float x, uint32_t id)
{
	x = fmaxf(x, 0.0f);
	switch (id) {
	case 1:
		return powf(x, 2.2f);
	case 2:
		return powf(x, 2.4f);
	case 3:
		return x <= 0.081f ? x / 4.5f : powf((x + 0.099f) / 1.099f, 1.0f / 0.45f);
	case 4:
		return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
	case 5:
		return x <= 0.5f ? (x * x) / 3.0f : (expf((x - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
	case 6: {
		const float p = powf(x, 1.0f / 78.84375f);
		return powf(fmaxf(p - 0.8359375f, 0.0f) / fmaxf(18.8515625f - 18.6875f * p, 1e-6f),
			    1.0f / 0.1593017578f);
	}
	default:
		return x;
	}
}

static float encode_transfer_cpu(float x, uint32_t id)
{
	x = fmaxf(x, 0.0f);
	switch (id) {
	case 1:
		return powf(x, 1.0f / 2.2f);
	case 2:
		return powf(x, 1.0f / 2.4f);
	case 3:
		return x <= 0.018f ? 4.5f * x : 1.099f * powf(x, 0.45f) - 0.099f;
	case 4:
		return x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
	case 5:
		return x <= (1.0f / 12.0f) ? sqrtf(3.0f * x)
					 : 0.17883277f * logf(fmaxf(12.0f * x - 0.28466892f, 1e-6f)) + 0.55991073f;
	case 6: {
		const float p = powf(x, 0.1593017578f);
		return powf((0.8359375f + 18.8515625f * p) /
				    fmaxf(1.0f + 18.6875f * p, 1e-6f),
				78.84375f);
	}
	default:
		return x;
	}
}

static void fill_transfer_lut(float *lut, uint32_t id, bool encode)
{
	for (int i = 0; i < D3D11_CST_LUT_SIZE; ++i) {
		const float x = (float)i / (float)(D3D11_CST_LUT_SIZE - 1);
		lut[i] = encode ? encode_transfer_cpu(x, id) : decode_transfer_cpu(x, id);
	}
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
	params->srcTransfer = source->transfer;
	params->targetTransfer = target->transfer;
	params->srcPrimaries = source->primaries;
	params->targetPrimaries = target->primaries;
	params->srcSlice = (UINT)(uintptr_t)input->data[1];
	params->width = (UINT)input->width;
	params->height = (UINT)input->height;
	params->srcFullRange = input_range == AVCOL_RANGE_JPEG ? 1.0f : 0.0f;
	params->peak = 1.0f;

	if (!cst->transfer_lut_valid || cst->cached_src_transfer != source->transfer ||
	    cst->cached_target_transfer != target->transfer) {
		fill_transfer_lut(cst->src_transfer_lut, source->transfer, false);
		fill_transfer_lut(cst->target_transfer_lut, target->transfer, true);
		cst->cached_src_transfer = source->transfer;
		cst->cached_target_transfer = target->transfer;
		cst->transfer_lut_valid = true;
	}

	memcpy(params->srcTransferLut, cst->src_transfer_lut, sizeof(params->srcTransferLut));
	memcpy(params->targetTransferLut, cst->target_transfer_lut, sizeof(params->targetTransferLut));

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

enum staging_copy_result {
	STAGING_COPY_ERROR = -1,
	STAGING_COPY_NOT_READY = 0,
	STAGING_COPY_DONE = 1,
};

static enum staging_copy_result copy_plane_to_frame_try(struct scrcpy_d3d11_cst *cst, ID3D11Texture2D *staging,
							uint8_t *dst, int dst_linesize, int height, size_t row_bytes)
{
	D3D11_MAPPED_SUBRESOURCE mapped = {0};
	HRESULT hr = ID3D11DeviceContext_Map(cst->context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ,
					     D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
	if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
		return STAGING_COPY_NOT_READY;
	if (FAILED(hr)) {
		log_hresult("Map(staging)", hr);
		return STAGING_COPY_ERROR;
	}

	for (int y = 0; y < height; ++y)
		memcpy(dst + (size_t)y * (size_t)dst_linesize,
		       (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch, row_bytes);

	ID3D11DeviceContext_Unmap(cst->context, (ID3D11Resource *)staging, 0);
	return STAGING_COPY_DONE;
}

static enum staging_copy_result copy_staging_to_frame(struct scrcpy_d3d11_cst *cst,
						      struct d3d11_cst_readback_slot *slot, AVFrame *output)
{
	const enum AVPixelFormat output_format = cst->output_10bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
	const size_t row_bytes = (size_t)cst->width * (cst->output_10bit ? 2U : 1U);
	enum staging_copy_result result;

	av_frame_unref(output);
	output->format = output_format;
	output->width = cst->width;
	output->height = cst->height;
	output->pts = slot->pts;
	if (av_frame_get_buffer(output, 32) < 0)
		return STAGING_COPY_ERROR;

	result = copy_plane_to_frame_try(cst, slot->staging_y_texture, output->data[0], output->linesize[0],
					 cst->height, row_bytes);
	if (result != STAGING_COPY_DONE)
		return result;

	result = copy_plane_to_frame_try(cst, slot->staging_uv_texture, output->data[1], output->linesize[1],
					 cst->height / 2, row_bytes);
	if (result != STAGING_COPY_DONE)
		return result;

	return STAGING_COPY_DONE;
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

	if (!cst->hw_device_ref || !compile_shader(cst->device, d3d11_cst_shader, false, &cst->shader_8bit) ||
	    !compile_shader(cst->device, d3d11_cst_shader, true, &cst->shader_10bit) || !create_params_buffer(cst)) {
		obs_log(LOG_WARNING, "scrcpy-d3d11-cst: initialization failed; using CPU CST fallback");
		scrcpy_d3d11_cst_destroy(cst);
		return NULL;
	}

	obs_log(LOG_INFO, "scrcpy-d3d11-cst: GPU CST initialized");
	return cst;
}

enum scrcpy_d3d11_cst_result scrcpy_d3d11_cst_apply(scrcpy_d3d11_cst_t *cst, const AVFrame *input, AVFrame *output,
						    enum AVColorRange input_range)
{
	struct gpu_profile source;
	struct gpu_profile target;
	struct d3d11_cst_readback_slot *submit_slot = NULL;
	ID3D11UnorderedAccessView *uavs[2];
	ID3D11ShaderResourceView *srvs[2];
	ID3D11UnorderedAccessView *null_uavs[2] = {NULL, NULL};
	ID3D11ShaderResourceView *null_srvs[2] = {NULL, NULL};
	ID3D11Buffer *null_buffer = NULL;
	ID3D11Buffer *cbuffers[1];
	ID3D11ComputeShader *shader;
	int completed = 0;

	if (!cst || cst->disabled || !input || !output)
		return SCRCPY_D3D11_CST_ERROR;

	if (!profile_from_frame(cst->source_profile, input, &source) ||
	    !profile_from_frame(cst->target_profile, NULL, &target))
		goto fail;

	if (!ensure_resources(cst, input))
		goto fail;

	/*
	 * A blocking Map immediately after CopyResource serialized the entire
	 * GPU pipeline with the decoder thread. Keep multiple readback slots in
	 * flight and poll staging resources without waiting.
	 */
	for (int i = 0; i < D3D11_CST_READBACK_SLOTS; ++i) {
		struct d3d11_cst_readback_slot *slot = &cst->slots[i];
		if (!slot->pending)
			continue;

		enum staging_copy_result result = copy_staging_to_frame(cst, slot, output);
		if (result == STAGING_COPY_DONE) {
			slot->pending = false;
			completed = 1;
			break;
		}
		if (result == STAGING_COPY_ERROR)
			goto fail;
	}

	if (completed) {
		output->color_primaries = target.av_primaries;
		output->colorspace = target.av_space;
		output->color_trc = target.av_trc;
		output->color_range = AVCOL_RANGE_JPEG;
	}

	for (int i = 0; i < D3D11_CST_READBACK_SLOTS; ++i) {
		if (!cst->slots[i].pending) {
			submit_slot = &cst->slots[i];
			break;
		}
	}

	if (!submit_slot)
		return completed ? SCRCPY_D3D11_CST_FRAME : SCRCPY_D3D11_CST_NO_FRAME;

	if (!update_params(cst, &source, &target, input, input_range))
		goto fail;

	srvs[0] = cst->input_y_srv;
	srvs[1] = cst->input_uv_srv;
	uavs[0] = submit_slot->output_y_uav;
	uavs[1] = submit_slot->output_uv_uav;
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

	ID3D11DeviceContext_CopyResource(cst->context, (ID3D11Resource *)submit_slot->staging_y_texture,
					 (ID3D11Resource *)submit_slot->output_y_texture);
	ID3D11DeviceContext_CopyResource(cst->context, (ID3D11Resource *)submit_slot->staging_uv_texture,
					 (ID3D11Resource *)submit_slot->output_uv_texture);

	submit_slot->pts = input->pts;
	submit_slot->pending = true;

	return completed ? SCRCPY_D3D11_CST_FRAME : SCRCPY_D3D11_CST_NO_FRAME;

fail:
	if (cst) {
		cst->disabled = true;
		obs_log(LOG_WARNING, "scrcpy-d3d11-cst: GPU path disabled; reverting to CPU CST");
	}
	return SCRCPY_D3D11_CST_ERROR;
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
