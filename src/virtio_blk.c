#include "virtio_blk.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

static void virtio_blk_notify(void *arg);

struct virtio_blk_ctx {
	struct virtio_mmio_dev *mmio;
	struct virtio_blk_device blk;
};

static void virtio_blk_notify(void *arg)
{
	struct virtio_blk_ctx *ctx = (struct virtio_blk_ctx *)arg;
	struct virtio_mmio_dev *mmio = ctx->mmio;
	struct virtio_blk_device *blk = &ctx->blk;

	uint16_t avail_idx;
	uint16_t *ring;
	uint16_t ring_size;
	int num_new = virtio_mmio_get_queue_avail(mmio, &avail_idx, &ring, &ring_size);

	printf("[VMM] virtio-blk notify: num_new=%d avail_idx=%u ring_size=%u\n",
	       num_new, avail_idx, ring_size);
	fflush(stdout);

	if (num_new <= 0)
		goto done;

	for (int i = 0; i < num_new; i++) {
		uint16_t desc_idx = ring[(avail_idx + i) % ring_size];
		uint64_t addr;
		uint32_t len;
		uint16_t flags, next;
		int status = VIRTIO_BLK_S_IOERR;
		uint32_t used_len = 0;

		if (virtio_mmio_get_desc(mmio, desc_idx, &addr, &len, &flags, &next) < 0)
			goto add_used;

		struct virtio_blk_outhdr hdr;
		if (virtio_mmio_read_desc_buf(mmio, desc_idx, &hdr, sizeof(hdr)) < 0)
			goto add_used;

		uint16_t data_desc = next;
		if (data_desc >= ring_size)
			goto add_used;

		uint64_t data_addr;
		uint32_t data_len;
		uint16_t data_flags, data_next;

		if (virtio_mmio_get_desc(mmio, data_desc, &data_addr, &data_len,
					 &data_flags, &data_next) < 0)
			goto add_used;

		uint16_t status_desc = data_next;
		if (status_desc >= ring_size)
			goto add_used;

		uint64_t offset = hdr.sector * blk->blk_size;
		if (offset + data_len > blk->image_size) {
			status = VIRTIO_BLK_S_IOERR;
			goto write_status;
		}

		if (hdr.type == VIRTIO_BLK_T_IN) {
			uint8_t *buf = malloc(data_len);
			if (!buf)
				goto write_status;

			ssize_t rd = pread(blk->fd, buf, data_len, (off_t)offset);
			if (rd != (ssize_t)data_len) {
				free(buf);
				goto write_status;
			}

			virtio_mmio_write_desc_buf(mmio, data_desc, buf, data_len);
			free(buf);
			status = VIRTIO_BLK_S_OK;
			used_len = data_len + 1;
		} else if (hdr.type == VIRTIO_BLK_T_OUT) {
			uint8_t *buf = malloc(data_len);
			if (!buf)
				goto write_status;

			virtio_mmio_read_desc_buf(mmio, data_desc, buf, data_len);
			ssize_t wr = pwrite(blk->fd, buf, data_len, (off_t)offset);
			free(buf);

			if (wr != (ssize_t)data_len)
				goto write_status;

			status = VIRTIO_BLK_S_OK;
			used_len = 1;
		} else if (hdr.type == VIRTIO_BLK_T_FLUSH) {
			if (fsync(blk->fd) < 0)
				goto write_status;
			status = VIRTIO_BLK_S_OK;
			used_len = 1;
		} else {
			status = VIRTIO_BLK_S_UNSUPP;
			used_len = 1;
		}

write_status:
		printf("[VMM] virtio-blk request: desc=%u type=%u sector=%llu len=%u used_len=%u status=%d\n",
		       desc_idx, hdr.type, (unsigned long long)hdr.sector,
		       data_len, used_len, status);
		fflush(stdout);
		virtio_mmio_write_desc_buf(mmio, status_desc, &status, 1);

	add_used:
		virtio_mmio_add_used(mmio, desc_idx, used_len);
	}

done:
	virtio_mmio_inject_irq(mmio);
}

struct virtio_blk_device *virtio_blk_init(const char *image_path)
{
	struct virtio_blk_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	int fd = open(image_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "[VMM] failed to open disk image %s: %d\n",
			image_path, errno);
		free(ctx);
		return NULL;
	}

	off_t size = lseek(fd, 0, SEEK_END);
	if (size < 0) {
		fprintf(stderr, "[VMM] failed to get disk image size: %d\n", errno);
		close(fd);
		free(ctx);
		return NULL;
	}

	lseek(fd, 0, SEEK_SET);

	ctx->blk.fd = fd;
	ctx->blk.image_size = (uint64_t)size;
	ctx->blk.blk_size = 512;

	memset(&ctx->blk.config, 0, sizeof(ctx->blk.config));
	ctx->blk.config.capacity = (uint64_t)size / ctx->blk.blk_size;
	ctx->blk.config.blk_size = ctx->blk.blk_size;
	ctx->blk.config.size_max = 0;
	ctx->blk.config.seg_max = 128 - 2;

	printf("[VMM] virtio-blk: image=%s, size=%ld bytes, capacity=%lu sectors\n",
	       image_path, (long)size, (unsigned long)ctx->blk.config.capacity);

	ctx->mmio = NULL;
	return &ctx->blk;
}

void virtio_blk_shutdown(struct virtio_blk_device *dev)
{
	if (!dev)
		return;

	close(dev->fd);

	struct virtio_blk_ctx *ctx = (struct virtio_blk_ctx *)
		((uintptr_t)dev - offsetof(struct virtio_blk_ctx, blk));
	free(ctx);
}

void *virtio_blk_get_config(struct virtio_blk_device *dev)
{
	if (!dev)
		return NULL;
	return &dev->config;
}

void virtio_blk_bind_mmio(struct virtio_blk_device *dev, struct virtio_mmio_dev *mmio)
{
	struct virtio_blk_ctx *ctx = (struct virtio_blk_ctx *)
		((uintptr_t)dev - offsetof(struct virtio_blk_ctx, blk));
	ctx->mmio = mmio;
	virtio_mmio_set_notify_cb(mmio, virtio_blk_notify, ctx);
}

void virtio_blk_handle_notify(struct virtio_mmio_dev *mmio, struct virtio_blk_device *blk)
{
	struct virtio_blk_ctx *ctx = (struct virtio_blk_ctx *)
		((uintptr_t)blk - offsetof(struct virtio_blk_ctx, blk));
	if (ctx->mmio == NULL) {
		ctx->mmio = mmio;
		virtio_mmio_set_notify_cb(mmio, virtio_blk_notify, ctx);
	}
	virtio_blk_notify(ctx);
}
