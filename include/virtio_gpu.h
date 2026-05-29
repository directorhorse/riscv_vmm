#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <linux/virtio_gpu.h>
#include <stdint.h>
#include "virtio_mmio.h"

#define VIRTIO_GPU_DEVICE_ID 16
#define VIRTIO_GPU_CONTROLQ 0
#define VIRTIO_GPU_CURSORQ 1
#define VIRTIO_GPU_NUM_QUEUES 2

struct virtio_gpu_device;

struct virtio_gpu_device *virtio_gpu_init(uint32_t width, uint32_t height);
void virtio_gpu_shutdown(struct virtio_gpu_device *gpu);
void *virtio_gpu_get_config(struct virtio_gpu_device *gpu);
void virtio_gpu_bind_mmio(struct virtio_gpu_device *gpu,
			  struct virtio_mmio_dev *mmio);

#endif
