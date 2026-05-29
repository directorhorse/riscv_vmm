#include "virtio_gpu.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIRTIO_GPU_MAX_RESOURCES 64
#define VIRTIO_GPU_MAX_BACKING 1024
#define VIRTIO_GPU_MAX_FB_BYTES (64U * 1024U * 1024U)
#define VIRTIO_GPU_BPP 4

struct virtio_gpu_resource {
	uint32_t id;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint8_t *fb;
	size_t fb_size;
	struct virtio_gpu_mem_entry *entries;
	uint32_t nr_entries;
};

struct virtio_gpu_device {
	struct virtio_mmio_dev *mmio;
	struct virtio_gpu_config config;
	uint32_t width;
	uint32_t height;
	uint32_t scanout_resource_id;
	struct virtio_gpu_resource resources[VIRTIO_GPU_MAX_RESOURCES];
};

static struct virtio_gpu_resource *find_resource(struct virtio_gpu_device *gpu,
						 uint32_t id)
{
	for (uint32_t i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
		if (gpu->resources[i].id == id)
			return &gpu->resources[i];
	}
	return NULL;
}

static struct virtio_gpu_resource *alloc_resource(struct virtio_gpu_device *gpu,
						  uint32_t id)
{
	for (uint32_t i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
		if (gpu->resources[i].id == 0) {
			gpu->resources[i].id = id;
			return &gpu->resources[i];
		}
	}
	return NULL;
}

static void free_resource(struct virtio_gpu_resource *res)
{
	free(res->fb);
	free(res->entries);
	memset(res, 0, sizeof(*res));
}

static int valid_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
		      uint32_t max_width, uint32_t max_height)
{
	return width && height &&
	       x <= max_width && y <= max_height &&
	       width <= max_width - x && height <= max_height - y;
}

static void make_resp_hdr(struct virtio_gpu_ctrl_hdr *resp,
			  const struct virtio_gpu_ctrl_hdr *req,
			  uint32_t type)
{
	memset(resp, 0, sizeof(*resp));
	resp->type = type;
	resp->flags = req->flags;
	resp->fence_id = req->fence_id;
	resp->ctx_id = req->ctx_id;
	resp->ring_idx = req->ring_idx;
}

static uint16_t find_writable_desc(struct virtio_mmio_dev *mmio,
				   uint32_t queue_index, uint16_t head_idx)
{
	uint16_t desc_idx = head_idx;

	while (1) {
		uint64_t addr;
		uint32_t len;
		uint16_t flags, next;

		if (virtio_mmio_get_desc(mmio, queue_index, desc_idx, &addr,
					 &len, &flags, &next) < 0)
			return UINT16_MAX;
		if (flags & VIRTQ_DESC_F_WRITE)
			return desc_idx;
		if (!(flags & VIRTQ_DESC_F_NEXT))
			return UINT16_MAX;
		desc_idx = next;
	}
}

static uint32_t write_response(struct virtio_mmio_dev *mmio,
			       uint32_t queue_index, uint16_t head_idx,
			       const void *resp, size_t len)
{
	uint16_t resp_desc = find_writable_desc(mmio, queue_index, head_idx);

	if (resp_desc == UINT16_MAX)
		return 0;
	if (virtio_mmio_write_desc_buf(mmio, queue_index, resp_desc,
				       resp, len) < 0)
		return 0;
	return (uint32_t)len;
}

static uint32_t respond_hdr(struct virtio_mmio_dev *mmio, uint32_t queue_index,
			    uint16_t head_idx,
			    const struct virtio_gpu_ctrl_hdr *req,
			    uint32_t type)
{
	struct virtio_gpu_ctrl_hdr resp;

	make_resp_hdr(&resp, req, type);
	return write_response(mmio, queue_index, head_idx, &resp, sizeof(resp));
}

static int backing_read(struct virtio_gpu_device *gpu,
			struct virtio_gpu_resource *res,
			uint64_t offset, void *buf, size_t len)
{
	size_t done = 0;

	(void)gpu;
	for (uint32_t i = 0; i < res->nr_entries && done < len; i++) {
		uint64_t start = res->entries[i].addr;
		uint32_t entry_len = res->entries[i].length;
		uint8_t *src;
		size_t chunk;

		if (offset >= entry_len) {
			offset -= entry_len;
			continue;
		}

		chunk = entry_len - offset;
		if (chunk > len - done)
			chunk = len - done;

		src = virtio_mmio_guest_to_host(gpu->mmio, start + offset);
		if (!src)
			return -1;

		memcpy((uint8_t *)buf + done, src, chunk);
		done += chunk;
		offset = 0;
	}

	return done == len ? 0 : -1;
}

static int transfer_to_host_2d(struct virtio_gpu_device *gpu,
			       struct virtio_gpu_resource *res,
			       const struct virtio_gpu_transfer_to_host_2d *cmd)
{
	uint32_t x = cmd->r.x;
	uint32_t y = cmd->r.y;
	uint32_t width = cmd->r.width;
	uint32_t height = cmd->r.height;
	uint64_t src_offset = cmd->offset;
	size_t row_bytes;
	size_t stride;

	if (!valid_rect(x, y, width, height, res->width, res->height))
		return -1;

	row_bytes = (size_t)width * VIRTIO_GPU_BPP;
	stride = (size_t)res->width * VIRTIO_GPU_BPP;
	for (uint32_t row = 0; row < height; row++) {
		size_t dst_offset = ((size_t)(y + row) * res->width + x) *
				    VIRTIO_GPU_BPP;
		if (backing_read(gpu, res, src_offset + row * stride,
				 res->fb + dst_offset, row_bytes) < 0)
			return -1;
	}

	return 0;
}

static uint32_t handle_get_display_info(struct virtio_gpu_device *gpu,
					uint32_t queue_index,
					uint16_t head_idx,
					const struct virtio_gpu_ctrl_hdr *req)
{
	struct virtio_gpu_resp_display_info resp;

	memset(&resp, 0, sizeof(resp));
	make_resp_hdr(&resp.hdr, req, VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
	resp.pmodes[0].r.width = gpu->width;
	resp.pmodes[0].r.height = gpu->height;
	resp.pmodes[0].enabled = 1;

	return write_response(gpu->mmio, queue_index, head_idx, &resp,
			      sizeof(resp));
}

static uint32_t handle_resource_create_2d(struct virtio_gpu_device *gpu,
					  uint32_t queue_index,
					  uint16_t head_idx,
					  const struct virtio_gpu_ctrl_hdr *req)
{
	struct virtio_gpu_resource_create_2d cmd;
	struct virtio_gpu_resource *res;
	size_t fb_size;

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx, 0,
					&cmd, sizeof(cmd)) < 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

	if (!cmd.resource_id || !cmd.width || !cmd.height ||
	    cmd.width > 8192 || cmd.height > 8192 ||
	    find_resource(gpu, cmd.resource_id))
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

	fb_size = (size_t)cmd.width * cmd.height * VIRTIO_GPU_BPP;
	if (fb_size > VIRTIO_GPU_MAX_FB_BYTES)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);

	res = alloc_resource(gpu, cmd.resource_id);
	if (!res)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);

	res->fb = calloc(1, fb_size);
	if (!res->fb) {
		free_resource(res);
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);
	}

	res->format = cmd.format;
	res->width = cmd.width;
	res->height = cmd.height;
	res->fb_size = fb_size;

	printf("[VMM] virtio-gpu create resource id=%u %ux%u format=%u\n",
	       res->id, res->width, res->height, res->format);
	fflush(stdout);

	return respond_hdr(gpu->mmio, queue_index, head_idx, req,
			   VIRTIO_GPU_RESP_OK_NODATA);
}

static uint32_t handle_resource_unref(struct virtio_gpu_device *gpu,
				      uint32_t queue_index, uint16_t head_idx,
				      const struct virtio_gpu_ctrl_hdr *req)
{
	struct virtio_gpu_resource_unref cmd;
	struct virtio_gpu_resource *res;

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx, 0,
					&cmd, sizeof(cmd)) < 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

	res = find_resource(gpu, cmd.resource_id);
	if (res) {
		if (gpu->scanout_resource_id == res->id)
			gpu->scanout_resource_id = 0;
		free_resource(res);
	}

	return respond_hdr(gpu->mmio, queue_index, head_idx, req,
			   VIRTIO_GPU_RESP_OK_NODATA);
}

static uint32_t handle_attach_backing(struct virtio_gpu_device *gpu,
				      uint32_t queue_index, uint16_t head_idx,
				      const struct virtio_gpu_ctrl_hdr *req)
{
	struct virtio_gpu_resource_attach_backing cmd;
	struct virtio_gpu_resource *res;
	size_t entries_size;

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx, 0,
					&cmd, sizeof(cmd)) < 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

	res = find_resource(gpu, cmd.resource_id);
	if (!res || !cmd.nr_entries || cmd.nr_entries > VIRTIO_GPU_MAX_BACKING)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

	entries_size = (size_t)cmd.nr_entries * sizeof(struct virtio_gpu_mem_entry);
	free(res->entries);
	res->entries = calloc(cmd.nr_entries, sizeof(*res->entries));
	if (!res->entries)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY);

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx,
					sizeof(cmd), res->entries,
					entries_size) < 0) {
		free(res->entries);
		res->entries = NULL;
		res->nr_entries = 0;
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
	}

	res->nr_entries = cmd.nr_entries;
	return respond_hdr(gpu->mmio, queue_index, head_idx, req,
			   VIRTIO_GPU_RESP_OK_NODATA);
}

static uint32_t handle_detach_backing(struct virtio_gpu_device *gpu,
				      uint32_t queue_index, uint16_t head_idx,
				      const struct virtio_gpu_ctrl_hdr *req)
{
	struct virtio_gpu_resource_detach_backing cmd;
	struct virtio_gpu_resource *res;

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx, 0,
					&cmd, sizeof(cmd)) < 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

	res = find_resource(gpu, cmd.resource_id);
	if (res) {
		free(res->entries);
		res->entries = NULL;
		res->nr_entries = 0;
	}

	return respond_hdr(gpu->mmio, queue_index, head_idx, req,
			   VIRTIO_GPU_RESP_OK_NODATA);
}

static uint32_t handle_set_scanout(struct virtio_gpu_device *gpu,
				   uint32_t queue_index, uint16_t head_idx,
				   const struct virtio_gpu_ctrl_hdr *req)
{
	struct virtio_gpu_set_scanout cmd;
	struct virtio_gpu_resource *res;

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx, 0,
					&cmd, sizeof(cmd)) < 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

	if (cmd.scanout_id != 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID);

	if (cmd.resource_id == 0) {
		gpu->scanout_resource_id = 0;
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_OK_NODATA);
	}

	res = find_resource(gpu, cmd.resource_id);
	if (!res || !valid_rect(cmd.r.x, cmd.r.y, cmd.r.width, cmd.r.height,
				res->width, res->height))
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

	gpu->scanout_resource_id = cmd.resource_id;
	return respond_hdr(gpu->mmio, queue_index, head_idx, req,
			   VIRTIO_GPU_RESP_OK_NODATA);
}

static uint32_t handle_transfer_to_host_2d(struct virtio_gpu_device *gpu,
					   uint32_t queue_index,
					   uint16_t head_idx,
					   const struct virtio_gpu_ctrl_hdr *req)
{
	struct virtio_gpu_transfer_to_host_2d cmd;
	struct virtio_gpu_resource *res;

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx, 0,
					&cmd, sizeof(cmd)) < 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

	res = find_resource(gpu, cmd.resource_id);
	if (!res || !res->entries ||
	    transfer_to_host_2d(gpu, res, &cmd) < 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

	return respond_hdr(gpu->mmio, queue_index, head_idx, req,
			   VIRTIO_GPU_RESP_OK_NODATA);
}

static uint32_t handle_resource_flush(struct virtio_gpu_device *gpu,
				      uint32_t queue_index, uint16_t head_idx,
				      const struct virtio_gpu_ctrl_hdr *req)
{
	struct virtio_gpu_resource_flush cmd;
	struct virtio_gpu_resource *res;

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx, 0,
					&cmd, sizeof(cmd)) < 0)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);

	res = find_resource(gpu, cmd.resource_id);
	if (!res)
		return respond_hdr(gpu->mmio, queue_index, head_idx, req,
				   VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID);

	printf("[VMM] virtio-gpu flush resource=%u rect=%u,%u %ux%u scanout=%u\n",
	       cmd.resource_id, cmd.r.x, cmd.r.y, cmd.r.width, cmd.r.height,
	       gpu->scanout_resource_id);
	fflush(stdout);

	return respond_hdr(gpu->mmio, queue_index, head_idx, req,
			   VIRTIO_GPU_RESP_OK_NODATA);
}

static uint32_t virtio_gpu_process_cmd(struct virtio_gpu_device *gpu,
				       uint32_t queue_index, uint16_t head_idx)
{
	struct virtio_gpu_ctrl_hdr hdr;

	if (virtio_mmio_read_desc_chain(gpu->mmio, queue_index, head_idx, 0,
					&hdr, sizeof(hdr)) < 0)
		return 0;

	switch (hdr.type) {
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
		return handle_get_display_info(gpu, queue_index, head_idx, &hdr);
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
		return handle_resource_create_2d(gpu, queue_index, head_idx, &hdr);
	case VIRTIO_GPU_CMD_RESOURCE_UNREF:
		return handle_resource_unref(gpu, queue_index, head_idx, &hdr);
	case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
		return handle_attach_backing(gpu, queue_index, head_idx, &hdr);
	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
		return handle_detach_backing(gpu, queue_index, head_idx, &hdr);
	case VIRTIO_GPU_CMD_SET_SCANOUT:
		return handle_set_scanout(gpu, queue_index, head_idx, &hdr);
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
		return handle_transfer_to_host_2d(gpu, queue_index, head_idx,
						  &hdr);
	case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
		return handle_resource_flush(gpu, queue_index, head_idx, &hdr);
	case VIRTIO_GPU_CMD_UPDATE_CURSOR:
	case VIRTIO_GPU_CMD_MOVE_CURSOR:
		return respond_hdr(gpu->mmio, queue_index, head_idx, &hdr,
				   VIRTIO_GPU_RESP_OK_NODATA);
	default:
		printf("[VMM] virtio-gpu unsupported cmd=0x%x queue=%u\n",
		       hdr.type, queue_index);
		fflush(stdout);
		return respond_hdr(gpu->mmio, queue_index, head_idx, &hdr,
				   VIRTIO_GPU_RESP_ERR_UNSPEC);
	}
}

static void virtio_gpu_notify(void *arg, uint32_t queue_index)
{
	struct virtio_gpu_device *gpu = (struct virtio_gpu_device *)arg;
	uint16_t avail_idx = 0;
	uint16_t *ring = NULL;
	uint16_t ring_size = 0;
	int num_new;

	if (queue_index >= VIRTIO_GPU_NUM_QUEUES)
		return;

	num_new = virtio_mmio_get_queue_avail(gpu->mmio, queue_index,
					      &avail_idx, &ring, &ring_size);
	if (num_new <= 0)
		return;

	for (int i = 0; i < num_new; i++) {
		uint16_t desc_idx = ring[(avail_idx + i) % ring_size];
		uint32_t used_len = virtio_gpu_process_cmd(gpu, queue_index,
							   desc_idx);
		virtio_mmio_add_used(gpu->mmio, queue_index, desc_idx,
				      used_len);
	}

	virtio_mmio_inject_irq(gpu->mmio);
}

struct virtio_gpu_device *virtio_gpu_init(uint32_t width, uint32_t height)
{
	struct virtio_gpu_device *gpu = calloc(1, sizeof(*gpu));

	if (!gpu)
		return NULL;
	if (!width)
		width = 1024;
	if (!height)
		height = 768;

	gpu->width = width;
	gpu->height = height;
	gpu->config.num_scanouts = 1;
	gpu->config.num_capsets = 0;

	return gpu;
}

void virtio_gpu_shutdown(struct virtio_gpu_device *gpu)
{
	if (!gpu)
		return;
	for (uint32_t i = 0; i < VIRTIO_GPU_MAX_RESOURCES; i++) {
		if (gpu->resources[i].id)
			free_resource(&gpu->resources[i]);
	}
	free(gpu);
}

void *virtio_gpu_get_config(struct virtio_gpu_device *gpu)
{
	if (!gpu)
		return NULL;
	return &gpu->config;
}

void virtio_gpu_bind_mmio(struct virtio_gpu_device *gpu,
			  struct virtio_mmio_dev *mmio)
{
	gpu->mmio = mmio;
	virtio_mmio_set_notify_cb(mmio, virtio_gpu_notify, gpu);
}
