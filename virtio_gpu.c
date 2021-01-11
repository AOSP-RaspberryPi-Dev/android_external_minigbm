/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "drv_priv.h"
#include "external/virgl_hw.h"
#include "external/virgl_protocol.h"
#include "external/virtgpu_drm.h"
#include "helpers.h"
#include "util.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif
#define PIPE_TEXTURE_2D 2

#define MESA_LLVMPIPE_TILE_ORDER 6
#define MESA_LLVMPIPE_TILE_SIZE (1 << MESA_LLVMPIPE_TILE_ORDER)

struct feature {
	uint64_t feature;
	const char *name;
	uint32_t enabled;
};

enum feature_id {
	feat_3d,
	feat_capset_fix,
	feat_resource_blob,
	feat_host_visible,
	feat_host_cross_device,
	feat_max,
};

#define FEATURE(x)                                                                                 \
	(struct feature)                                                                           \
	{                                                                                          \
		x, #x, 0                                                                           \
	}

static struct feature features[] = {
	FEATURE(VIRTGPU_PARAM_3D_FEATURES),   FEATURE(VIRTGPU_PARAM_CAPSET_QUERY_FIX),
	FEATURE(VIRTGPU_PARAM_RESOURCE_BLOB), FEATURE(VIRTGPU_PARAM_HOST_VISIBLE),
	FEATURE(VIRTGPU_PARAM_CROSS_DEVICE),
};

static const uint32_t render_target_formats[] = { DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB8888,
						  DRM_FORMAT_RGB565, DRM_FORMAT_XBGR8888,
						  DRM_FORMAT_XRGB8888 };

static const uint32_t dumb_texture_source_formats[] = {
	DRM_FORMAT_R8,	 DRM_FORMAT_R16,  DRM_FORMAT_YVU420,
	DRM_FORMAT_NV12, DRM_FORMAT_NV21, DRM_FORMAT_YVU420_ANDROID
};

static const uint32_t texture_source_formats[] = { DRM_FORMAT_NV12, DRM_FORMAT_NV21,
						   DRM_FORMAT_R8,   DRM_FORMAT_R16,
						   DRM_FORMAT_RG88, DRM_FORMAT_YVU420_ANDROID };

struct virtio_gpu_priv {
	int caps_is_v2;
	union virgl_caps caps;
	int host_gbm_enabled;
	atomic_int next_blob_id;
};

static uint32_t translate_format(uint32_t drm_fourcc)
{
	switch (drm_fourcc) {
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_RGB888:
		return VIRGL_FORMAT_R8G8B8_UNORM;
	case DRM_FORMAT_XRGB8888:
		return VIRGL_FORMAT_B8G8R8X8_UNORM;
	case DRM_FORMAT_ARGB8888:
		return VIRGL_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_XBGR8888:
		return VIRGL_FORMAT_R8G8B8X8_UNORM;
	case DRM_FORMAT_ABGR8888:
		return VIRGL_FORMAT_R8G8B8A8_UNORM;
	case DRM_FORMAT_ABGR16161616F:
		return VIRGL_FORMAT_R16G16B16A16_FLOAT;
	case DRM_FORMAT_RGB565:
		return VIRGL_FORMAT_B5G6R5_UNORM;
	case DRM_FORMAT_R8:
		return VIRGL_FORMAT_R8_UNORM;
	case DRM_FORMAT_RG88:
		return VIRGL_FORMAT_R8G8_UNORM;
	case DRM_FORMAT_NV12:
		return VIRGL_FORMAT_NV12;
	case DRM_FORMAT_NV21:
		return VIRGL_FORMAT_NV21;
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU420_ANDROID:
		return VIRGL_FORMAT_YV12;
	default:
		return 0;
	}
}

static bool virtio_gpu_bitmask_supports_format(struct virgl_supported_format_mask *supported,
					       uint32_t drm_format)
{
	uint32_t virgl_format = translate_format(drm_format);
	if (!virgl_format) {
		return false;
	}

	uint32_t bitmask_index = virgl_format / 32;
	uint32_t bit_index = virgl_format % 32;
	return supported->bitmask[bitmask_index] & (1 << bit_index);
}

// The metadata generated here for emulated buffers is slightly different than the metadata
// generated by drv_bo_from_format. In order to simplify transfers in the flush and invalidate
// functions below, the emulated buffers are oversized. For example, ignoring stride alignment
// requirements to demonstrate, a 6x6 YUV420 image buffer might have the following layout from
// drv_bo_from_format:
//
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | U | U | U | U | U | U |
// | U | U | U | V | V | V |
// | V | V | V | V | V | V |
//
// where each plane immediately follows the previous plane in memory. This layout makes it
// difficult to compute the transfers needed for example when the middle 2x2 region of the
// image is locked and needs to be flushed/invalidated.
//
// Emulated multi-plane buffers instead have a layout of:
//
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | Y | Y | Y | Y | Y | Y |
// | U | U | U |   |   |   |
// | U | U | U |   |   |   |
// | U | U | U |   |   |   |
// | V | V | V |   |   |   |
// | V | V | V |   |   |   |
// | V | V | V |   |   |   |
//
// where each plane is placed as a sub-image (albeit with a very large stride) in order to
// simplify transfers into 3 sub-image transfers for the above example.
//
// Additional note: the V-plane is not placed to the right of the U-plane due to some
// observed failures in media framework code which assumes the V-plane is not
// "row-interlaced" with the U-plane.
static void virtio_gpu_get_emulated_metadata(const struct bo *bo, struct bo_metadata *metadata)
{
	uint32_t y_plane_height;
	uint32_t c_plane_height;
	uint32_t original_width = bo->meta.width;
	uint32_t original_height = bo->meta.height;

	metadata->format = DRM_FORMAT_R8;
	switch (bo->meta.format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		// Bi-planar
		metadata->num_planes = 2;

		y_plane_height = original_height;
		c_plane_height = DIV_ROUND_UP(original_height, 2);

		metadata->width = original_width;
		metadata->height = y_plane_height + c_plane_height;

		// Y-plane (full resolution)
		metadata->strides[0] = metadata->width;
		metadata->offsets[0] = 0;
		metadata->sizes[0] = metadata->width * y_plane_height;

		// CbCr-plane  (half resolution, interleaved, placed below Y-plane)
		metadata->strides[1] = metadata->width;
		metadata->offsets[1] = metadata->offsets[0] + metadata->sizes[0];
		metadata->sizes[1] = metadata->width * c_plane_height;

		metadata->total_size = metadata->width * metadata->height;
		break;
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU420_ANDROID:
		// Tri-planar
		metadata->num_planes = 3;

		y_plane_height = original_height;
		c_plane_height = DIV_ROUND_UP(original_height, 2);

		metadata->width = ALIGN(original_width, 32);
		metadata->height = y_plane_height + (2 * c_plane_height);

		// Y-plane (full resolution)
		metadata->strides[0] = metadata->width;
		metadata->offsets[0] = 0;
		metadata->sizes[0] = metadata->width * original_height;

		// Cb-plane (half resolution, placed below Y-plane)
		metadata->strides[1] = metadata->width;
		metadata->offsets[1] = metadata->offsets[0] + metadata->sizes[0];
		metadata->sizes[1] = metadata->width * c_plane_height;

		// Cr-plane (half resolution, placed below Cb-plane)
		metadata->strides[2] = metadata->width;
		metadata->offsets[2] = metadata->offsets[1] + metadata->sizes[1];
		metadata->sizes[2] = metadata->width * c_plane_height;

		metadata->total_size = metadata->width * metadata->height;
		break;
	default:
		break;
	}
}

struct virtio_transfers_params {
	size_t xfers_needed;
	struct rectangle xfer_boxes[DRV_MAX_PLANES];
};

static void virtio_gpu_get_emulated_transfers_params(const struct bo *bo,
						     const struct rectangle *transfer_box,
						     struct virtio_transfers_params *xfer_params)
{
	uint32_t y_plane_height;
	uint32_t c_plane_height;
	struct bo_metadata emulated_metadata;

	if (transfer_box->x == 0 && transfer_box->y == 0 && transfer_box->width == bo->meta.width &&
	    transfer_box->height == bo->meta.height) {
		virtio_gpu_get_emulated_metadata(bo, &emulated_metadata);

		xfer_params->xfers_needed = 1;
		xfer_params->xfer_boxes[0].x = 0;
		xfer_params->xfer_boxes[0].y = 0;
		xfer_params->xfer_boxes[0].width = emulated_metadata.width;
		xfer_params->xfer_boxes[0].height = emulated_metadata.height;

		return;
	}

	switch (bo->meta.format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		// Bi-planar
		xfer_params->xfers_needed = 2;

		y_plane_height = bo->meta.height;
		c_plane_height = DIV_ROUND_UP(bo->meta.height, 2);

		// Y-plane (full resolution)
		xfer_params->xfer_boxes[0].x = transfer_box->x;
		xfer_params->xfer_boxes[0].y = transfer_box->y;
		xfer_params->xfer_boxes[0].width = transfer_box->width;
		xfer_params->xfer_boxes[0].height = transfer_box->height;

		// CbCr-plane (half resolution, interleaved, placed below Y-plane)
		xfer_params->xfer_boxes[1].x = transfer_box->x;
		xfer_params->xfer_boxes[1].y = transfer_box->y + y_plane_height;
		xfer_params->xfer_boxes[1].width = transfer_box->width;
		xfer_params->xfer_boxes[1].height = DIV_ROUND_UP(transfer_box->height, 2);

		break;
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_YVU420_ANDROID:
		// Tri-planar
		xfer_params->xfers_needed = 3;

		y_plane_height = bo->meta.height;
		c_plane_height = DIV_ROUND_UP(bo->meta.height, 2);

		// Y-plane (full resolution)
		xfer_params->xfer_boxes[0].x = transfer_box->x;
		xfer_params->xfer_boxes[0].y = transfer_box->y;
		xfer_params->xfer_boxes[0].width = transfer_box->width;
		xfer_params->xfer_boxes[0].height = transfer_box->height;

		// Cb-plane (half resolution, placed below Y-plane)
		xfer_params->xfer_boxes[1].x = transfer_box->x;
		xfer_params->xfer_boxes[1].y = transfer_box->y + y_plane_height;
		xfer_params->xfer_boxes[1].width = DIV_ROUND_UP(transfer_box->width, 2);
		xfer_params->xfer_boxes[1].height = DIV_ROUND_UP(transfer_box->height, 2);

		// Cr-plane (half resolution, placed below Cb-plane)
		xfer_params->xfer_boxes[2].x = transfer_box->x;
		xfer_params->xfer_boxes[2].y = transfer_box->y + y_plane_height + c_plane_height;
		xfer_params->xfer_boxes[2].width = DIV_ROUND_UP(transfer_box->width, 2);
		xfer_params->xfer_boxes[2].height = DIV_ROUND_UP(transfer_box->height, 2);

		break;
	}
}

static bool virtio_gpu_supports_combination_natively(struct driver *drv, uint32_t drm_format,
						     uint64_t use_flags)
{
	struct virtio_gpu_priv *priv = (struct virtio_gpu_priv *)drv->priv;

	if (priv->caps.max_version == 0) {
		return true;
	}

	if ((use_flags & BO_USE_RENDERING) &&
	    !virtio_gpu_bitmask_supports_format(&priv->caps.v1.render, drm_format)) {
		return false;
	}

	if ((use_flags & BO_USE_TEXTURE) &&
	    !virtio_gpu_bitmask_supports_format(&priv->caps.v1.sampler, drm_format)) {
		return false;
	}

	if ((use_flags & BO_USE_SCANOUT) && priv->caps_is_v2 &&
	    !virtio_gpu_bitmask_supports_format(&priv->caps.v2.scanout, drm_format)) {
		return false;
	}

	return true;
}

// For virtio backends that do not support formats natively (e.g. multi-planar formats are not
// supported in virglrenderer when gbm is unavailable on the host machine), whether or not the
// format and usage combination can be handled as a blob (byte buffer).
static bool virtio_gpu_supports_combination_through_emulation(struct driver *drv,
							      uint32_t drm_format,
							      uint64_t use_flags)
{
	struct virtio_gpu_priv *priv = (struct virtio_gpu_priv *)drv->priv;

	// Only enable emulation on non-gbm virtio backends.
	if (priv->host_gbm_enabled) {
		return false;
	}

	if (use_flags & (BO_USE_RENDERING | BO_USE_SCANOUT)) {
		return false;
	}

	if (!virtio_gpu_supports_combination_natively(drv, DRM_FORMAT_R8, use_flags)) {
		return false;
	}

	return drm_format == DRM_FORMAT_NV12 || drm_format == DRM_FORMAT_NV21 ||
	       drm_format == DRM_FORMAT_YVU420 || drm_format == DRM_FORMAT_YVU420_ANDROID;
}

// Adds the given buffer combination to the list of supported buffer combinations if the
// combination is supported by the virtio backend.
static void virtio_gpu_add_combination(struct driver *drv, uint32_t drm_format,
				       struct format_metadata *metadata, uint64_t use_flags)
{
	struct virtio_gpu_priv *priv = (struct virtio_gpu_priv *)drv->priv;

	if (features[feat_3d].enabled && priv->caps.max_version >= 1) {
		if ((use_flags & BO_USE_SCANOUT) && priv->caps_is_v2 &&
		    !virtio_gpu_supports_combination_natively(drv, drm_format, use_flags)) {
			drv_log("Scanout format: %d\n", drm_format);
			use_flags &= ~BO_USE_SCANOUT;
		}

		if (!virtio_gpu_supports_combination_natively(drv, drm_format, use_flags) &&
		    !virtio_gpu_supports_combination_through_emulation(drv, drm_format,
								       use_flags)) {
			drv_log("Skipping unsupported combination format:%d\n", drm_format);
			return;
		}
	}

	drv_add_combination(drv, drm_format, metadata, use_flags);
}

// Adds each given buffer combination to the list of supported buffer combinations if the
// combination supported by the virtio backend.
static void virtio_gpu_add_combinations(struct driver *drv, const uint32_t *drm_formats,
					uint32_t num_formats, struct format_metadata *metadata,
					uint64_t use_flags)
{
	uint32_t i;

	for (i = 0; i < num_formats; i++) {
		virtio_gpu_add_combination(drv, drm_formats[i], metadata, use_flags);
	}
}

static int virtio_dumb_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				 uint64_t use_flags)
{
	if (bo->meta.format != DRM_FORMAT_R8) {
		width = ALIGN(width, MESA_LLVMPIPE_TILE_SIZE);
		height = ALIGN(height, MESA_LLVMPIPE_TILE_SIZE);
	}

	return drv_dumb_bo_create_ex(bo, width, height, format, use_flags, BO_QUIRK_DUMB32BPP);
}

static inline void handle_flag(uint64_t *flag, uint64_t check_flag, uint32_t *bind,
			       uint32_t virgl_bind)
{
	if ((*flag) & check_flag) {
		(*flag) &= ~check_flag;
		(*bind) |= virgl_bind;
	}
}

static uint32_t compute_virgl_bind_flags(uint64_t use_flags, uint32_t format)
{
	/* In crosvm, VIRGL_BIND_SHARED means minigbm will allocate, not virglrenderer. */
	uint32_t bind = VIRGL_BIND_SHARED;

	handle_flag(&use_flags, BO_USE_TEXTURE, &bind, VIRGL_BIND_SAMPLER_VIEW);
	handle_flag(&use_flags, BO_USE_RENDERING, &bind, VIRGL_BIND_RENDER_TARGET);
	handle_flag(&use_flags, BO_USE_SCANOUT, &bind, VIRGL_BIND_SCANOUT);
	handle_flag(&use_flags, BO_USE_CURSOR, &bind, VIRGL_BIND_CURSOR);
	handle_flag(&use_flags, BO_USE_LINEAR, &bind, VIRGL_BIND_LINEAR);

	if (use_flags & BO_USE_PROTECTED) {
		handle_flag(&use_flags, BO_USE_PROTECTED, &bind, VIRGL_BIND_MINIGBM_PROTECTED);
	} else {
		// Make sure we don't set both flags, since that could be mistaken for
		// protected. Give OFTEN priority over RARELY.
		if (use_flags & BO_USE_SW_READ_OFTEN) {
			handle_flag(&use_flags, BO_USE_SW_READ_OFTEN, &bind,
				    VIRGL_BIND_MINIGBM_SW_READ_OFTEN);
		} else {
			handle_flag(&use_flags, BO_USE_SW_READ_RARELY, &bind,
				    VIRGL_BIND_MINIGBM_SW_READ_RARELY);
		}
		if (use_flags & BO_USE_SW_WRITE_OFTEN) {
			handle_flag(&use_flags, BO_USE_SW_WRITE_OFTEN, &bind,
				    VIRGL_BIND_MINIGBM_SW_WRITE_OFTEN);
		} else {
			handle_flag(&use_flags, BO_USE_SW_WRITE_RARELY, &bind,
				    VIRGL_BIND_MINIGBM_SW_WRITE_RARELY);
		}
	}

	handle_flag(&use_flags, BO_USE_CAMERA_WRITE, &bind, VIRGL_BIND_MINIGBM_CAMERA_WRITE);
	handle_flag(&use_flags, BO_USE_CAMERA_READ, &bind, VIRGL_BIND_MINIGBM_CAMERA_READ);
	handle_flag(&use_flags, BO_USE_HW_VIDEO_DECODER, &bind,
		    VIRGL_BIND_MINIGBM_HW_VIDEO_DECODER);
	handle_flag(&use_flags, BO_USE_HW_VIDEO_ENCODER, &bind,
		    VIRGL_BIND_MINIGBM_HW_VIDEO_ENCODER);

	/*
	 * HACK: This is for HAL_PIXEL_FORMAT_YV12 buffers allocated by arcvm. None of
	 * our platforms can display YV12, so we can treat as a SW buffer. Remove once
	 * this can be intelligently resolved in the guest. Also see gbm_bo_create.
	 */
	if (format == DRM_FORMAT_YVU420_ANDROID) {
		bind |= VIRGL_BIND_LINEAR;
	}

	if (use_flags) {
		drv_log("Unhandled bo use flag: %llx\n", (unsigned long long)use_flags);
	}

	return bind;
}

static int virtio_virgl_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				  uint64_t use_flags)
{
	int ret;
	size_t i;
	uint32_t stride;
	struct drm_virtgpu_resource_create res_create = { 0 };
	struct bo_metadata emulated_metadata;

	if (virtio_gpu_supports_combination_natively(bo->drv, format, use_flags)) {
		stride = drv_stride_from_format(format, width, 0);
		drv_bo_from_format(bo, stride, height, format);
	} else {
		assert(
		    virtio_gpu_supports_combination_through_emulation(bo->drv, format, use_flags));

		virtio_gpu_get_emulated_metadata(bo, &emulated_metadata);

		format = emulated_metadata.format;
		width = emulated_metadata.width;
		height = emulated_metadata.height;
		for (i = 0; i < emulated_metadata.num_planes; i++) {
			bo->meta.strides[i] = emulated_metadata.strides[i];
			bo->meta.offsets[i] = emulated_metadata.offsets[i];
			bo->meta.sizes[i] = emulated_metadata.sizes[i];
		}
		bo->meta.total_size = emulated_metadata.total_size;
	}

	/*
	 * Setting the target is intended to ensure this resource gets bound as a 2D
	 * texture in the host renderer's GL state. All of these resource properties are
	 * sent unchanged by the kernel to the host, which in turn sends them unchanged to
	 * virglrenderer. When virglrenderer makes a resource, it will convert the target
	 * enum to the equivalent one in GL and then bind the resource to that target.
	 */

	res_create.target = PIPE_TEXTURE_2D;
	res_create.format = translate_format(format);
	res_create.bind = compute_virgl_bind_flags(use_flags, format);
	res_create.width = width;
	res_create.height = height;

	/* For virgl 3D */
	res_create.depth = 1;
	res_create.array_size = 1;
	res_create.last_level = 0;
	res_create.nr_samples = 0;

	res_create.size = ALIGN(bo->meta.total_size, PAGE_SIZE); // PAGE_SIZE = 0x1000
	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &res_create);
	if (ret) {
		drv_log("DRM_IOCTL_VIRTGPU_RESOURCE_CREATE failed with %s\n", strerror(errno));
		return ret;
	}

	for (uint32_t plane = 0; plane < bo->meta.num_planes; plane++)
		bo->handles[plane].u32 = res_create.bo_handle;

	return 0;
}

static void *virtio_virgl_bo_map(struct bo *bo, struct vma *vma, size_t plane, uint32_t map_flags)
{
	int ret;
	struct drm_virtgpu_map gem_map = { 0 };

	gem_map.handle = bo->handles[0].u32;
	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VIRTGPU_MAP, &gem_map);
	if (ret) {
		drv_log("DRM_IOCTL_VIRTGPU_MAP failed with %s\n", strerror(errno));
		return MAP_FAILED;
	}

	vma->length = bo->meta.total_size;
	return mmap(0, bo->meta.total_size, drv_get_prot(map_flags), MAP_SHARED, bo->drv->fd,
		    gem_map.offset);
}

static int virtio_gpu_get_caps(struct driver *drv, union virgl_caps *caps, int *caps_is_v2)
{
	int ret;
	struct drm_virtgpu_get_caps cap_args = { 0 };

	*caps_is_v2 = 0;
	cap_args.addr = (unsigned long long)caps;
	if (features[feat_capset_fix].enabled) {
		*caps_is_v2 = 1;
		cap_args.cap_set_id = 2;
		cap_args.size = sizeof(union virgl_caps);
	} else {
		cap_args.cap_set_id = 1;
		cap_args.size = sizeof(struct virgl_caps_v1);
	}

	ret = drmIoctl(drv->fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &cap_args);
	if (ret) {
		drv_log("DRM_IOCTL_VIRTGPU_GET_CAPS failed with %s\n", strerror(errno));
		*caps_is_v2 = 0;

		// Fallback to v1
		cap_args.cap_set_id = 1;
		cap_args.size = sizeof(struct virgl_caps_v1);

		ret = drmIoctl(drv->fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &cap_args);
		if (ret) {
			drv_log("DRM_IOCTL_VIRTGPU_GET_CAPS failed with %s\n", strerror(errno));
		}
	}

	return ret;
}

static void virtio_gpu_init_features_and_caps(struct driver *drv)
{
	struct virtio_gpu_priv *priv = (struct virtio_gpu_priv *)drv->priv;

	for (uint32_t i = 0; i < ARRAY_SIZE(features); i++) {
		struct drm_virtgpu_getparam params = { 0 };

		params.param = features[i].feature;
		params.value = (uint64_t)(uintptr_t)&features[i].enabled;
		int ret = drmIoctl(drv->fd, DRM_IOCTL_VIRTGPU_GETPARAM, &params);
		if (ret)
			drv_log("DRM_IOCTL_VIRTGPU_GET_PARAM failed with %s\n", strerror(errno));
	}

	if (features[feat_3d].enabled) {
		virtio_gpu_get_caps(drv, &priv->caps, &priv->caps_is_v2);
	}

	// Multi-planar formats are currently only supported in virglrenderer through gbm.
	priv->host_gbm_enabled =
	    virtio_gpu_supports_combination_natively(drv, DRM_FORMAT_NV12, BO_USE_TEXTURE);
}

static int virtio_gpu_init(struct driver *drv)
{
	struct virtio_gpu_priv *priv;

	priv = calloc(1, sizeof(*priv));
	drv->priv = priv;

	virtio_gpu_init_features_and_caps(drv);

	if (features[feat_3d].enabled) {
		/* This doesn't mean host can scanout everything, it just means host
		 * hypervisor can show it. */
		virtio_gpu_add_combinations(drv, render_target_formats,
					    ARRAY_SIZE(render_target_formats), &LINEAR_METADATA,
					    BO_USE_RENDER_MASK | BO_USE_SCANOUT);
		virtio_gpu_add_combinations(drv, texture_source_formats,
					    ARRAY_SIZE(texture_source_formats), &LINEAR_METADATA,
					    BO_USE_TEXTURE_MASK);
	} else {
		/* Virtio primary plane only allows this format. */
		virtio_gpu_add_combination(drv, DRM_FORMAT_XRGB8888, &LINEAR_METADATA,
					   BO_USE_RENDER_MASK | BO_USE_SCANOUT);
		/* Virtio cursor plane only allows this format and Chrome cannot live without
		 * ARGB888 renderable format. */
		virtio_gpu_add_combination(drv, DRM_FORMAT_ARGB8888, &LINEAR_METADATA,
					   BO_USE_RENDER_MASK | BO_USE_CURSOR);
		/* Android needs more, but they cannot be bound as scanouts anymore after
		 * "drm/virtio: fix DRM_FORMAT_* handling" */
		virtio_gpu_add_combinations(drv, render_target_formats,
					    ARRAY_SIZE(render_target_formats), &LINEAR_METADATA,
					    BO_USE_RENDER_MASK);
		virtio_gpu_add_combinations(drv, dumb_texture_source_formats,
					    ARRAY_SIZE(dumb_texture_source_formats),
					    &LINEAR_METADATA, BO_USE_TEXTURE_MASK);
		virtio_gpu_add_combination(drv, DRM_FORMAT_NV12, &LINEAR_METADATA,
					   BO_USE_SW_MASK | BO_USE_LINEAR);
		virtio_gpu_add_combination(drv, DRM_FORMAT_NV21, &LINEAR_METADATA,
					   BO_USE_SW_MASK | BO_USE_LINEAR);
	}

	/* Android CTS tests require this. */
	virtio_gpu_add_combination(drv, DRM_FORMAT_RGB888, &LINEAR_METADATA, BO_USE_SW_MASK);
	virtio_gpu_add_combination(drv, DRM_FORMAT_BGR888, &LINEAR_METADATA, BO_USE_SW_MASK);
	virtio_gpu_add_combination(drv, DRM_FORMAT_ABGR16161616F, &LINEAR_METADATA,
				   BO_USE_SW_MASK | BO_USE_TEXTURE_MASK);

	drv_modify_combination(drv, DRM_FORMAT_NV12, &LINEAR_METADATA,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_HW_VIDEO_ENCODER);
	drv_modify_combination(drv, DRM_FORMAT_R8, &LINEAR_METADATA,
			       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE | BO_USE_HW_VIDEO_DECODER |
				   BO_USE_HW_VIDEO_ENCODER);

	if (!priv->host_gbm_enabled) {
		drv_modify_combination(drv, DRM_FORMAT_ABGR8888, &LINEAR_METADATA,
				       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
					   BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER);
		drv_modify_combination(drv, DRM_FORMAT_XBGR8888, &LINEAR_METADATA,
				       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
					   BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER);
		drv_modify_combination(drv, DRM_FORMAT_NV21, &LINEAR_METADATA,
				       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
					   BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER);
		drv_modify_combination(drv, DRM_FORMAT_R16, &LINEAR_METADATA,
				       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
					   BO_USE_HW_VIDEO_DECODER);
		drv_modify_combination(drv, DRM_FORMAT_YVU420, &LINEAR_METADATA,
				       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
					   BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER);
		drv_modify_combination(drv, DRM_FORMAT_YVU420_ANDROID, &LINEAR_METADATA,
				       BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE |
					   BO_USE_HW_VIDEO_DECODER | BO_USE_HW_VIDEO_ENCODER);
	}

	return drv_modify_linear_combinations(drv);
}

static void virtio_gpu_close(struct driver *drv)
{
	free(drv->priv);
	drv->priv = NULL;
}

static int virtio_gpu_bo_create_blob(struct driver *drv, struct bo *bo)
{
	int ret;
	uint32_t stride;
	uint32_t cur_blob_id;
	uint32_t cmd[VIRGL_PIPE_RES_CREATE_SIZE + 1] = { 0 };
	struct drm_virtgpu_resource_create_blob drm_rc_blob = { 0 };
	struct virtio_gpu_priv *priv = (struct virtio_gpu_priv *)drv->priv;

	uint32_t blob_flags = VIRTGPU_BLOB_FLAG_USE_SHAREABLE;
	if (bo->meta.use_flags & BO_USE_SW_MASK)
		blob_flags |= VIRTGPU_BLOB_FLAG_USE_MAPPABLE;
	if (bo->meta.use_flags & BO_USE_NON_GPU_HW)
		blob_flags |= VIRTGPU_BLOB_FLAG_USE_CROSS_DEVICE;

	cur_blob_id = atomic_fetch_add(&priv->next_blob_id, 1);
	stride = drv_stride_from_format(bo->meta.format, bo->meta.width, 0);
	drv_bo_from_format(bo, stride, bo->meta.height, bo->meta.format);
	bo->meta.total_size = ALIGN(bo->meta.total_size, PAGE_SIZE);
	bo->meta.tiling = blob_flags;

	cmd[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_CREATE, 0, VIRGL_PIPE_RES_CREATE_SIZE);
	cmd[VIRGL_PIPE_RES_CREATE_TARGET] = PIPE_TEXTURE_2D;
	cmd[VIRGL_PIPE_RES_CREATE_WIDTH] = bo->meta.width;
	cmd[VIRGL_PIPE_RES_CREATE_HEIGHT] = bo->meta.height;
	cmd[VIRGL_PIPE_RES_CREATE_FORMAT] = translate_format(bo->meta.format);
	cmd[VIRGL_PIPE_RES_CREATE_BIND] =
	    compute_virgl_bind_flags(bo->meta.use_flags, bo->meta.format);
	cmd[VIRGL_PIPE_RES_CREATE_DEPTH] = 1;
	cmd[VIRGL_PIPE_RES_CREATE_BLOB_ID] = cur_blob_id;

	drm_rc_blob.cmd = (uint64_t)&cmd;
	drm_rc_blob.cmd_size = 4 * (VIRGL_PIPE_RES_CREATE_SIZE + 1);
	drm_rc_blob.size = bo->meta.total_size;
	drm_rc_blob.blob_mem = VIRTGPU_BLOB_MEM_HOST3D;
	drm_rc_blob.blob_flags = blob_flags;
	drm_rc_blob.blob_id = cur_blob_id;

	ret = drmIoctl(drv->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &drm_rc_blob);
	if (ret < 0) {
		drv_log("DRM_VIRTGPU_RESOURCE_CREATE_BLOB failed with %s\n", strerror(errno));
		return -errno;
	}

	for (uint32_t plane = 0; plane < bo->meta.num_planes; plane++)
		bo->handles[plane].u32 = drm_rc_blob.bo_handle;

	return 0;
}

static bool should_use_blob(struct driver *drv, uint32_t format, uint64_t use_flags)
{
	struct virtio_gpu_priv *priv = (struct virtio_gpu_priv *)drv->priv;

	// TODO(gurchetansingh): remove once all minigbm users are blob-safe
#ifndef VIRTIO_GPU_NEXT
	return false;
#endif

	// Only use blob when host gbm is available
	if (!priv->host_gbm_enabled)
		return false;

	// Use regular resources if only the GPU needs efficient access
	if (!(use_flags &
	      (BO_USE_SW_READ_OFTEN | BO_USE_SW_WRITE_OFTEN | BO_USE_LINEAR | BO_USE_NON_GPU_HW)))
		return false;

	switch (format) {
	case DRM_FORMAT_YVU420_ANDROID:
	case DRM_FORMAT_R8:
		// Formats with strictly defined strides are supported
		return true;
	case DRM_FORMAT_NV12:
		// Knowing buffer metadata at buffer creation isn't yet supported, so buffers
		// can't be properly mapped into the guest.
		return (use_flags & BO_USE_SW_MASK) == 0;
	default:
		return false;
	}
}

static int virtio_gpu_bo_create(struct bo *bo, uint32_t width, uint32_t height, uint32_t format,
				uint64_t use_flags)
{
	if (features[feat_resource_blob].enabled && features[feat_host_visible].enabled &&
	    should_use_blob(bo->drv, format, use_flags))
		return virtio_gpu_bo_create_blob(bo->drv, bo);

	if (features[feat_3d].enabled)
		return virtio_virgl_bo_create(bo, width, height, format, use_flags);
	else
		return virtio_dumb_bo_create(bo, width, height, format, use_flags);
}

static int virtio_gpu_bo_destroy(struct bo *bo)
{
	if (features[feat_3d].enabled)
		return drv_gem_bo_destroy(bo);
	else
		return drv_dumb_bo_destroy(bo);
}

static void *virtio_gpu_bo_map(struct bo *bo, struct vma *vma, size_t plane, uint32_t map_flags)
{
	if (features[feat_3d].enabled)
		return virtio_virgl_bo_map(bo, vma, plane, map_flags);
	else
		return drv_dumb_bo_map(bo, vma, plane, map_flags);
}

static int virtio_gpu_bo_invalidate(struct bo *bo, struct mapping *mapping)
{
	int ret;
	size_t i;
	struct drm_virtgpu_3d_transfer_from_host xfer = { 0 };
	struct drm_virtgpu_3d_wait waitcmd = { 0 };
	struct virtio_transfers_params xfer_params;
	struct virtio_gpu_priv *priv = (struct virtio_gpu_priv *)bo->drv->priv;
	uint64_t host_write_flags;

	if (!features[feat_3d].enabled)
		return 0;

	// Invalidate is only necessary if the host writes to the buffer. The encoder and
	// decoder flags don't differentiate between input and output buffers, but we can
	// use the format to determine whether this buffer could be encoder/decoder output.
	host_write_flags = BO_USE_RENDERING | BO_USE_CAMERA_WRITE;
	if (bo->meta.format == DRM_FORMAT_R8) {
		host_write_flags |= BO_USE_HW_VIDEO_ENCODER;
	} else {
		host_write_flags |= BO_USE_HW_VIDEO_DECODER;
	}
	if ((bo->meta.use_flags & host_write_flags) == 0)
		return 0;

	if (features[feat_resource_blob].enabled &&
	    (bo->meta.tiling & VIRTGPU_BLOB_FLAG_USE_MAPPABLE))
		return 0;

	xfer.bo_handle = mapping->vma->handle;

	if (mapping->rect.x || mapping->rect.y) {
		/*
		 * virglrenderer uses the box parameters and assumes that offset == 0 for planar
		 * images
		 */
		if (bo->meta.num_planes == 1) {
			xfer.offset =
			    (bo->meta.strides[0] * mapping->rect.y) +
			    drv_bytes_per_pixel_from_format(bo->meta.format, 0) * mapping->rect.x;
		}
	}

	if ((bo->meta.use_flags & BO_USE_RENDERING) == 0) {
		// Unfortunately, the kernel doesn't actually pass the guest layer_stride
		// and guest stride to the host (compare virtio_gpu.h and virtgpu_drm.h).
		// For gbm based resources, we can work around this by using the level field
		// to pass the stride to virglrenderer's gbm transfer code. However, we need
		// to avoid doing this for resources which don't rely on that transfer code,
		// which is resources with the BO_USE_RENDERING flag set.
		// TODO(b/145993887): Send also stride when the patches are landed
		if (priv->host_gbm_enabled) {
			xfer.level = bo->meta.strides[0];
		}
	}

	if (virtio_gpu_supports_combination_natively(bo->drv, bo->meta.format,
						     bo->meta.use_flags)) {
		xfer_params.xfers_needed = 1;
		xfer_params.xfer_boxes[0] = mapping->rect;
	} else {
		assert(virtio_gpu_supports_combination_through_emulation(bo->drv, bo->meta.format,
									 bo->meta.use_flags));

		virtio_gpu_get_emulated_transfers_params(bo, &mapping->rect, &xfer_params);
	}

	for (i = 0; i < xfer_params.xfers_needed; i++) {
		xfer.box.x = xfer_params.xfer_boxes[i].x;
		xfer.box.y = xfer_params.xfer_boxes[i].y;
		xfer.box.w = xfer_params.xfer_boxes[i].width;
		xfer.box.h = xfer_params.xfer_boxes[i].height;
		xfer.box.d = 1;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &xfer);
		if (ret) {
			drv_log("DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST failed with %s\n",
				strerror(errno));
			return -errno;
		}
	}

	// The transfer needs to complete before invalidate returns so that any host changes
	// are visible and to ensure the host doesn't overwrite subsequent guest changes.
	// TODO(b/136733358): Support returning fences from transfers
	waitcmd.handle = mapping->vma->handle;
	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VIRTGPU_WAIT, &waitcmd);
	if (ret) {
		drv_log("DRM_IOCTL_VIRTGPU_WAIT failed with %s\n", strerror(errno));
		return -errno;
	}

	return 0;
}

static int virtio_gpu_bo_flush(struct bo *bo, struct mapping *mapping)
{
	int ret;
	size_t i;
	struct drm_virtgpu_3d_transfer_to_host xfer = { 0 };
	struct drm_virtgpu_3d_wait waitcmd = { 0 };
	struct virtio_transfers_params xfer_params;
	struct virtio_gpu_priv *priv = (struct virtio_gpu_priv *)bo->drv->priv;

	if (!features[feat_3d].enabled)
		return 0;

	if (!(mapping->vma->map_flags & BO_MAP_WRITE))
		return 0;

	if (features[feat_resource_blob].enabled &&
	    (bo->meta.tiling & VIRTGPU_BLOB_FLAG_USE_MAPPABLE))
		return 0;

	xfer.bo_handle = mapping->vma->handle;

	if (mapping->rect.x || mapping->rect.y) {
		/*
		 * virglrenderer uses the box parameters and assumes that offset == 0 for planar
		 * images
		 */
		if (bo->meta.num_planes == 1) {
			xfer.offset =
			    (bo->meta.strides[0] * mapping->rect.y) +
			    drv_bytes_per_pixel_from_format(bo->meta.format, 0) * mapping->rect.x;
		}
	}

	// Unfortunately, the kernel doesn't actually pass the guest layer_stride and
	// guest stride to the host (compare virtio_gpu.h and virtgpu_drm.h). We can use
	// the level to work around this.
	if (priv->host_gbm_enabled) {
		xfer.level = bo->meta.strides[0];
	}

	if (virtio_gpu_supports_combination_natively(bo->drv, bo->meta.format,
						     bo->meta.use_flags)) {
		xfer_params.xfers_needed = 1;
		xfer_params.xfer_boxes[0] = mapping->rect;
	} else {
		assert(virtio_gpu_supports_combination_through_emulation(bo->drv, bo->meta.format,
									 bo->meta.use_flags));

		virtio_gpu_get_emulated_transfers_params(bo, &mapping->rect, &xfer_params);
	}

	for (i = 0; i < xfer_params.xfers_needed; i++) {
		xfer.box.x = xfer_params.xfer_boxes[i].x;
		xfer.box.y = xfer_params.xfer_boxes[i].y;
		xfer.box.w = xfer_params.xfer_boxes[i].width;
		xfer.box.h = xfer_params.xfer_boxes[i].height;
		xfer.box.d = 1;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &xfer);
		if (ret) {
			drv_log("DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST failed with %s\n",
				strerror(errno));
			return -errno;
		}
	}

	// If the buffer is only accessed by the host GPU, then the flush is ordered
	// with subsequent commands. However, if other host hardware can access the
	// buffer, we need to wait for the transfer to complete for consistency.
	// TODO(b/136733358): Support returning fences from transfers
	if (bo->meta.use_flags & BO_USE_NON_GPU_HW) {
		waitcmd.handle = mapping->vma->handle;

		ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VIRTGPU_WAIT, &waitcmd);
		if (ret) {
			drv_log("DRM_IOCTL_VIRTGPU_WAIT failed with %s\n", strerror(errno));
			return -errno;
		}
	}

	return 0;
}

static uint32_t virtio_gpu_resolve_format(struct driver *drv, uint32_t format, uint64_t use_flags)
{
	switch (format) {
	case DRM_FORMAT_FLEX_IMPLEMENTATION_DEFINED:
		/* Camera subsystem requires NV12. */
		if (use_flags & (BO_USE_CAMERA_READ | BO_USE_CAMERA_WRITE))
			return DRM_FORMAT_NV12;
		/*HACK: See b/28671744 */
		return DRM_FORMAT_XBGR8888;
	case DRM_FORMAT_FLEX_YCbCr_420_888:
		/*
		 * All of our host drivers prefer NV12 as their flexible media format.
		 * If that changes, this will need to be modified.
		 */
		if (features[feat_3d].enabled)
			return DRM_FORMAT_NV12;
		else
			return DRM_FORMAT_YVU420_ANDROID;
	default:
		return format;
	}
}

static int virtio_gpu_resource_info(struct bo *bo, uint32_t strides[DRV_MAX_PLANES],
				    uint32_t offsets[DRV_MAX_PLANES])
{
	int ret;
	struct drm_virtgpu_resource_info_cros res_info = { 0 };

	if (!features[feat_3d].enabled)
		return 0;

	res_info.bo_handle = bo->handles[0].u32;
	ret = drmIoctl(bo->drv->fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO_CROS, &res_info);
	if (ret) {
		drv_log("DRM_IOCTL_VIRTGPU_RESOURCE_INFO failed with %s\n", strerror(errno));
		return ret;
	}

	for (uint32_t plane = 0; plane < bo->meta.num_planes; plane++) {
		/*
		 * Currently, kernel v4.14 (Betty) doesn't have the extended resource info
		 * ioctl.
		 */
		if (res_info.strides[plane]) {
			strides[plane] = res_info.strides[plane];
			offsets[plane] = res_info.offsets[plane];
		}
	}

	return 0;
}

const struct backend backend_virtio_gpu = {
	.name = "virtio_gpu",
	.init = virtio_gpu_init,
	.close = virtio_gpu_close,
	.bo_create = virtio_gpu_bo_create,
	.bo_destroy = virtio_gpu_bo_destroy,
	.bo_import = drv_prime_bo_import,
	.bo_map = virtio_gpu_bo_map,
	.bo_unmap = drv_bo_munmap,
	.bo_invalidate = virtio_gpu_bo_invalidate,
	.bo_flush = virtio_gpu_bo_flush,
	.resolve_format = virtio_gpu_resolve_format,
	.resource_info = virtio_gpu_resource_info,
};
