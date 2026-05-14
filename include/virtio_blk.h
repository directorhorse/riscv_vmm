#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>
#include "virtio_mmio.h"

#define VIRTIO_BLK_F_SIZE_MAX       (1 << 1)
#define VIRTIO_BLK_F_SEG_MAX        (1 << 2)
#define VIRTIO_BLK_F_GEOMETRY       (1 << 4)
#define VIRTIO_BLK_F_RO             (1 << 5)
#define VIRTIO_BLK_F_BLK_SIZE       (1 << 6)
#define VIRTIO_BLK_F_FLUSH          (1 << 9)
#define VIRTIO_BLK_F_TOPOLOGY       (1 << 10)
#define VIRTIO_BLK_F_CONFIG_WCE     (1 << 11)

#define VIRTIO_BLK_T_IN             0
#define VIRTIO_BLK_T_OUT            1
#define VIRTIO_BLK_T_FLUSH          4
#define VIRTIO_BLK_T_DISCARD        11
#define VIRTIO_BLK_T_WRITE_ZEROES   13

#define VIRTIO_BLK_S_OK             0
#define VIRTIO_BLK_S_IOERR          1
#define VIRTIO_BLK_S_UNSUPP         2

struct virtio_blk_outhdr {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
};

struct virtio_blk_config {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;
    struct {
        uint16_t cylinders;
        uint8_t heads;
        uint8_t sectors;
    } geometry;
    uint32_t blk_size;
    uint16_t min_io_size;
    uint32_t opt_io_size;
    uint8_t writeback;
    uint8_t unused;
    uint32_t discard_sector_start;
    uint32_t discard_sector_count;
    uint32_t discard_alignment;
    uint32_t write_zeroes_may_unmap;
} __attribute__((packed));

struct virtio_blk_device {
    int fd;
    uint64_t image_size;
    uint32_t blk_size;
    struct virtio_blk_config config;
};

struct virtio_blk_device *virtio_blk_init(const char *image_path);
void virtio_blk_shutdown(struct virtio_blk_device *dev);
void virtio_blk_bind_mmio(struct virtio_blk_device *dev, struct virtio_mmio_dev *mmio);
void *virtio_blk_get_config(struct virtio_blk_device *dev);
void virtio_blk_handle_notify(struct virtio_mmio_dev *mmio, struct virtio_blk_device *blk);

#endif
