#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

#include <stdint.h>
#include <stddef.h>

#define VIRTIO_MMIO_MAGIC           0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00c
#define VIRTIO_MMIO_HOST_FEATURES   0x010
#define VIRTIO_MMIO_HOST_FEATURES_SEL 0x014
#define VIRTIO_MMIO_GUEST_FEATURES  0x020
#define VIRTIO_MMIO_GUEST_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM      0x038
#define VIRTIO_MMIO_QUEUE_READY     0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0a0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0fc
#define VIRTIO_MMIO_CONFIG           0x100

#define VIRTIO_MMIO_MAGIC_VALUE     0x74726976
#define VIRTIO_MMIO_VERSION_2       2

#define VIRTIO_F_VERSION_1          32

#define VIRTIO_MMIO_IRQ_VQ          0x01
#define VIRTIO_MMIO_IRQ_CONFIG      0x02

#define VIRTIO_DEV_ANY              0xffffffff

struct virtio_mmio_dev;

struct virtio_mmio_dev *virtio_mmio_init(uint64_t base_addr,
	void *guest_mem_base, uint64_t guest_mem_start, int vcpu_fd,
	uint32_t device_id, uint32_t vendor_id,
	uint32_t host_features[2], uint32_t queue_num_max,
	void *dev_config, uint32_t dev_config_size);

void virtio_mmio_set_notify_cb(struct virtio_mmio_dev *dev,
	void (*cb)(void *), void *notify_dev);

void virtio_mmio_set_plic_pending(struct virtio_mmio_dev *dev,
	uint32_t *plic_pending);

int virtio_mmio_handle_access(struct virtio_mmio_dev *dev,
	uint64_t phys_addr, uint8_t *data, uint32_t len, int is_write);

int virtio_mmio_get_queue_avail(struct virtio_mmio_dev *dev,
	uint16_t *out_idx, uint16_t **ring_start, uint16_t *ring_size);

int virtio_mmio_get_desc(struct virtio_mmio_dev *dev, uint16_t desc_idx,
	uint64_t *addr, uint32_t *len, uint16_t *flags, uint16_t *next);

int virtio_mmio_read_desc_buf(struct virtio_mmio_dev *dev,
	uint16_t desc_idx, void *buf, size_t len);

int virtio_mmio_write_desc_buf(struct virtio_mmio_dev *dev,
	uint16_t desc_idx, const void *buf, size_t len);

void virtio_mmio_add_used(struct virtio_mmio_dev *dev,
	uint16_t id, uint32_t len);

void virtio_mmio_inject_irq(struct virtio_mmio_dev *dev);

#endif
