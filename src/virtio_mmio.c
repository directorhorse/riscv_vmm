#include "virtio_mmio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <asm/kvm.h>

struct virtq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

#define VIRTQ_DESC_F_NEXT    1
#define VIRTQ_DESC_F_WRITE   2

struct virtq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];
};

struct virtq_used_elem {
	uint32_t id;
	uint32_t len;
};

struct virtq_used {
	uint16_t flags;
	uint16_t idx;
	struct virtq_used_elem ring[];
};

#define MAX_VIRTQ_NUM 256
#define VIRTIO_BLK_IRQ 2

struct virtio_mmio_dev {
	uint32_t host_features[2];
	uint32_t host_features_sel;
	uint32_t guest_features[2];
	uint32_t guest_features_sel;
	uint32_t queue_sel;
	uint32_t queue_num;
	uint64_t queue_desc;
	uint64_t queue_driver;
	uint64_t queue_device;
	uint32_t status;
	uint32_t interrupt_status;
	uint32_t config_generation;

	struct virtq_desc *desc;
	struct virtq_avail *avail;
	struct virtq_used *used;
	uint32_t queue_num_max;
	uint16_t last_used_idx;
	int queue_ready;

	uint32_t device_id;
	uint32_t vendor_id;

	void *guest_mem_base;
	uint64_t guest_mem_start;
	uint64_t mmio_base;

	int vcpu_fd;

	void (*notify_cb)(void *dev);
	void *notify_dev;

	uint32_t *plic_pending;

	void *dev_config;
	uint32_t dev_config_size;
};

static void *gpa_to_hva(struct virtio_mmio_dev *dev, uint64_t gpa)
{
	if (gpa < dev->guest_mem_start)
		return NULL;
	uint64_t offset = gpa - dev->guest_mem_start;
	return (void *)((uintptr_t)dev->guest_mem_base + offset);
}

static int queue_supported(struct virtio_mmio_dev *dev)
{
	return dev->queue_sel == 0;
}

static uint64_t virtio_mmio_reg_read(struct virtio_mmio_dev *dev, uint32_t offset)
{
	switch (offset) {
	case VIRTIO_MMIO_MAGIC:
		return VIRTIO_MMIO_MAGIC_VALUE;
	case VIRTIO_MMIO_VERSION:
		return VIRTIO_MMIO_VERSION_2;
	case VIRTIO_MMIO_DEVICE_ID:
		return dev->device_id;
	case VIRTIO_MMIO_VENDOR_ID:
		return dev->vendor_id;
	case VIRTIO_MMIO_HOST_FEATURES:
		return dev->host_features[dev->host_features_sel];
	case VIRTIO_MMIO_HOST_FEATURES_SEL:
		return dev->host_features_sel;
	case VIRTIO_MMIO_GUEST_FEATURES:
		return dev->guest_features[dev->guest_features_sel];
	case VIRTIO_MMIO_GUEST_FEATURES_SEL:
		return dev->guest_features_sel;
	case VIRTIO_MMIO_QUEUE_SEL:
		return dev->queue_sel;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		return queue_supported(dev) ? dev->queue_num_max : 0;
	case VIRTIO_MMIO_QUEUE_NUM:
		return queue_supported(dev) ? dev->queue_num : 0;
	case VIRTIO_MMIO_QUEUE_READY:
		return queue_supported(dev) ? dev->queue_ready : 0;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		return 0;
	case VIRTIO_MMIO_INTERRUPT_STATUS:
		return dev->interrupt_status;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		return 0;
	case VIRTIO_MMIO_STATUS:
		return dev->status;
	case VIRTIO_MMIO_QUEUE_DESC_LOW:
		return (uint32_t)dev->queue_desc;
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:
		return (uint32_t)(dev->queue_desc >> 32);
	case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
		return (uint32_t)dev->queue_driver;
	case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
		return (uint32_t)(dev->queue_driver >> 32);
	case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
		return (uint32_t)dev->queue_device;
	case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
		return (uint32_t)(dev->queue_device >> 32);
	case VIRTIO_MMIO_CONFIG_GENERATION:
		return dev->config_generation;
	default:
		break;
	}

	return 0;
}

static void setup_virtqueue(struct virtio_mmio_dev *dev)
{
	if (!queue_supported(dev) || !dev->queue_num ||
	    dev->queue_num > dev->queue_num_max)
		return;

	dev->desc = gpa_to_hva(dev, dev->queue_desc);
	dev->avail = gpa_to_hva(dev, dev->queue_driver);
	dev->used = gpa_to_hva(dev, dev->queue_device);
	if (!dev->desc || !dev->avail || !dev->used)
		return;

	dev->last_used_idx = 0;
	dev->queue_ready = 1;
}

static void virtio_mmio_reset(struct virtio_mmio_dev *dev)
{
	dev->host_features_sel = 0;
	dev->guest_features[0] = 0;
	dev->guest_features[1] = 0;
	dev->guest_features_sel = 0;
	dev->queue_sel = 0;
	dev->queue_num = 0;
	dev->queue_desc = 0;
	dev->queue_driver = 0;
	dev->queue_device = 0;
	dev->status = 0;
	dev->interrupt_status = 0;
	dev->desc = NULL;
	dev->avail = NULL;
	dev->used = NULL;
	dev->last_used_idx = 0;
	dev->queue_ready = 0;
}

static void virtio_mmio_reg_write(struct virtio_mmio_dev *dev, uint32_t offset,
				  uint32_t val)
{
	switch (offset) {
	case VIRTIO_MMIO_HOST_FEATURES_SEL:
		dev->host_features_sel = val & 1;
		break;
	case VIRTIO_MMIO_GUEST_FEATURES:
		dev->guest_features[dev->guest_features_sel] = val;
		break;
	case VIRTIO_MMIO_GUEST_FEATURES_SEL:
		dev->guest_features_sel = val & 1;
		break;
	case VIRTIO_MMIO_QUEUE_SEL:
		dev->queue_sel = val;
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		if (queue_supported(dev))
			dev->queue_num = val;
		break;
	case VIRTIO_MMIO_QUEUE_READY:
		if (!queue_supported(dev))
			break;
		dev->queue_ready = 0;
		if (val)
			setup_virtqueue(dev);
		break;
	case VIRTIO_MMIO_QUEUE_DESC_LOW:
		if (queue_supported(dev))
			dev->queue_desc = (dev->queue_desc & 0xffffffff00000000ULL) | val;
		break;
	case VIRTIO_MMIO_QUEUE_DESC_HIGH:
		if (queue_supported(dev))
			dev->queue_desc = (dev->queue_desc & 0xffffffffULL) |
				((uint64_t)val << 32);
		break;
	case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
		if (queue_supported(dev))
			dev->queue_driver = (dev->queue_driver & 0xffffffff00000000ULL) | val;
		break;
	case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
		if (queue_supported(dev))
			dev->queue_driver = (dev->queue_driver & 0xffffffffULL) |
				((uint64_t)val << 32);
		break;
	case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
		if (queue_supported(dev))
			dev->queue_device = (dev->queue_device & 0xffffffff00000000ULL) | val;
		break;
	case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
		if (queue_supported(dev))
			dev->queue_device = (dev->queue_device & 0xffffffffULL) |
				((uint64_t)val << 32);
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		if (dev->notify_cb)
			dev->notify_cb(dev->notify_dev);
		break;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		dev->interrupt_status &= ~val;
		break;
	case VIRTIO_MMIO_STATUS:
		dev->status = val;
		if (val == 0)
			virtio_mmio_reset(dev);
		break;
	default:
		break;
	}
}

struct virtio_mmio_dev *virtio_mmio_init(uint64_t base_addr,
					 void *guest_mem_base,
					 uint64_t guest_mem_start,
					 int vcpu_fd,
					 uint32_t device_id,
					 uint32_t vendor_id,
					 uint32_t host_features[2],
					 uint32_t queue_num_max,
					 void *dev_config,
					 uint32_t dev_config_size)
{
	struct virtio_mmio_dev *dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	virtio_mmio_reset(dev);

	dev->mmio_base = base_addr;
	dev->guest_mem_base = guest_mem_base;
	dev->guest_mem_start = guest_mem_start;
	dev->vcpu_fd = vcpu_fd;
	dev->device_id = device_id;
	dev->vendor_id = vendor_id;
	dev->queue_num_max = queue_num_max;
	dev->config_generation = 1;

	if (host_features) {
		dev->host_features[0] = host_features[0];
		dev->host_features[1] = host_features[1];
	}
	dev->host_features[1] |= 1U << (VIRTIO_F_VERSION_1 - 32);

	dev->dev_config = dev_config;
	dev->dev_config_size = dev_config_size;

	return dev;
}

void virtio_mmio_set_notify_cb(struct virtio_mmio_dev *dev,
			       void (*cb)(void *), void *notify_dev)
{
	dev->notify_cb = cb;
	dev->notify_dev = notify_dev;
}

void virtio_mmio_set_plic_pending(struct virtio_mmio_dev *dev,
				  uint32_t *plic_pending)
{
	dev->plic_pending = plic_pending;
}

static uint32_t extract_data(const uint8_t *data, uint32_t len)
{
	uint32_t val = 0;
	for (uint32_t i = 0; i < len && i < 4; i++)
		val |= ((uint32_t)data[i]) << (i * 8);
	return val;
}

static void store_data(uint8_t *data, uint32_t len, uint64_t val)
{
	for (uint32_t i = 0; i < len && i < 8; i++)
		data[i] = (val >> (i * 8)) & 0xff;
	for (uint32_t i = len; i < 8; i++)
		data[i] = 0;
}

static void read_config(struct virtio_mmio_dev *dev, uint32_t offset,
			uint8_t *data, uint32_t len)
{
	memset(data, 0, len);

	if (!dev->dev_config || offset < VIRTIO_MMIO_CONFIG)
		return;

	uint32_t config_off = offset - VIRTIO_MMIO_CONFIG;
	if (config_off >= dev->dev_config_size)
		return;

	uint32_t copy_len = dev->dev_config_size - config_off;
	if (copy_len > len)
		copy_len = len;
	memcpy(data, (uint8_t *)dev->dev_config + config_off, copy_len);
}

int virtio_mmio_handle_access(struct virtio_mmio_dev *dev,
			      uint64_t phys_addr,
			      uint8_t *data, uint32_t len, int is_write)
{
	if (phys_addr < dev->mmio_base ||
	    phys_addr >= dev->mmio_base + VIRTIO_MMIO_CONFIG + 0x1000)
		return 0;

	uint32_t offset = (uint32_t)(phys_addr - dev->mmio_base);

	if (len > 8)
		len = 8;

	if (is_write) {
		uint32_t val = extract_data(data, len);
		printf("[VMM] virtio mmio write: off=0x%x val=0x%x len=%d\n",
		      offset, val, len);
		virtio_mmio_reg_write(dev, offset, val);
	} else {
		if (offset >= VIRTIO_MMIO_CONFIG) {
			read_config(dev, offset, data, len);
			printf("[VMM] virtio mmio read:  off=0x%x len=%d\n",
			       offset, len);
		} else {
			uint64_t val = virtio_mmio_reg_read(dev, offset);
			printf("[VMM] virtio mmio read:  off=0x%x val=0x%llx len=%d\n",
			       offset, (unsigned long long)val, len);
			store_data(data, len, val);
		}
	}

	return 1;
}

int virtio_mmio_get_queue_avail(struct virtio_mmio_dev *dev,
				uint16_t *out_idx,
				uint16_t **ring_start, uint16_t *ring_size)
{
	if (!dev->queue_ready)
		return -1;

	uint16_t avail_idx = dev->avail->idx;
	uint16_t num_new = avail_idx - dev->last_used_idx;

	if (num_new == 0)
		return 0;

	*out_idx = dev->last_used_idx;
	*ring_start = dev->avail->ring;
	*ring_size = dev->queue_num;

	return (int)num_new;
}

int virtio_mmio_get_desc(struct virtio_mmio_dev *dev, uint16_t desc_idx,
			 uint64_t *addr, uint32_t *len,
			 uint16_t *flags, uint16_t *next)
{
	if (!dev->queue_ready || desc_idx >= dev->queue_num)
		return -1;

	*addr = dev->desc[desc_idx].addr;
	*len = dev->desc[desc_idx].len;
	*flags = dev->desc[desc_idx].flags;
	*next = dev->desc[desc_idx].next;
	return 0;
}

int virtio_mmio_read_desc_buf(struct virtio_mmio_dev *dev,
			      uint16_t desc_idx, void *buf, size_t len)
{
	uint64_t addr;
	uint32_t desc_len;
	uint16_t flags, next;

	if (virtio_mmio_get_desc(dev, desc_idx, &addr, &desc_len, &flags, &next) < 0)
		return -1;

	if (len > desc_len)
		len = desc_len;

	void *src = gpa_to_hva(dev, addr);
	if (!src)
		return -1;

	memcpy(buf, src, len);
	return 0;
}

int virtio_mmio_write_desc_buf(struct virtio_mmio_dev *dev,
			       uint16_t desc_idx, const void *buf, size_t len)
{
	uint64_t addr;
	uint32_t desc_len;
	uint16_t flags, next;

	if (virtio_mmio_get_desc(dev, desc_idx, &addr, &desc_len, &flags, &next) < 0)
		return -1;

	if (len > desc_len)
		len = desc_len;

	void *dst = gpa_to_hva(dev, addr);
	if (!dst)
		return -1;

	memcpy(dst, buf, len);
	return 0;
}

void virtio_mmio_add_used(struct virtio_mmio_dev *dev,
			  uint16_t id, uint32_t len)
{
	if (!dev->queue_ready)
		return;

	uint16_t used_idx = dev->used->idx;
	dev->used->ring[used_idx % dev->queue_num].id = id;
	dev->used->ring[used_idx % dev->queue_num].len = len;

	__sync_synchronize();

	dev->used->idx = used_idx + 1;
	dev->last_used_idx++;
}

void virtio_mmio_inject_irq(struct virtio_mmio_dev *dev)
{
	if (!(dev->status & 0x04))
		return;

	dev->interrupt_status |= VIRTIO_MMIO_IRQ_VQ;

	if (dev->plic_pending) {
		__sync_fetch_and_or(dev->plic_pending, 1U << VIRTIO_BLK_IRQ);
		__sync_synchronize();
	}

	struct kvm_interrupt irq = { .irq = KVM_INTERRUPT_SET };
	if (ioctl(dev->vcpu_fd, KVM_INTERRUPT, &irq) < 0)
		perror("[VMM] KVM_INTERRUPT SET failed");
	printf("[VMM] virtio irq inject: plic_irq=%d interrupt_status=0x%x\n",
	       VIRTIO_BLK_IRQ, dev->interrupt_status);
	fflush(stdout);
}
