#include "vmm.h"
#include "virtio_mmio.h"
#include "virtio_blk.h"
#include "virtio_gpu.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <asm/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <termios.h>

#define VIRTIO_BLK_MMIO_BASE 0x10001000
#define VIRTIO_BLK_IRQ 2
#define VIRTIO_GPU_MMIO_BASE 0x10002000
#define VIRTIO_GPU_IRQ 3
#define VIRTIO_GPU_WIDTH 1024
#define VIRTIO_GPU_HEIGHT 768
#define MAX_VIRTIO_MMIO_DEVS 8

#define UART_RX_FIFO_SIZE 256

static struct {
    uint8_t rbr, ier, fcr, lcr, mcr, lsr, msr, scr, dll, dlh;
    int thre_ack;
    uint8_t rx_fifo[UART_RX_FIFO_SIZE];
    int rx_head, rx_tail, rx_count;
} uart;

static struct termios orig_tio;

static void term_restore(void) {
    tcsetattr(0, TCSAFLUSH, &orig_tio);
}

static void term_raw(void) {
    tcgetattr(0, &orig_tio);
    atexit(term_restore);
    struct termios raw = orig_tio;
    raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);
}

static void uart_update_irq(struct init_struct *init) {
    // int pending = 0;
    // if ((uart.ier & 0x04) && (uart.lsr & 0x1e)) pending = 1;
    // if (!pending && (uart.ier & 0x01) && (uart.lsr & 0x01)) pending = 1;
    // if (!pending && (uart.ier & 0x02) && !uart.thre_ack) pending = 1;
    // if (!pending && (uart.ier & 0x08) && (uart.msr & 0x0f)) pending = 1;

    // struct kvm_irq_level irq = { .irq = 1, .level = pending ? 1 : 0 };
    // ioctl(init->vm_fd, KVM_IRQ_LINE, &irq);
}

static void uart_write(uint8_t reg, uint8_t val, struct init_struct *init) {
    switch (reg) {
    case 0:
        if (uart.lcr & 0x80) {
            uart.dll = val;
        } else {
            putchar(val);
            fflush(stdout);
            uart.thre_ack = 0;
        }
        break;
    case 1:
        if (uart.lcr & 0x80) uart.dlh = val;
        else                 uart.ier = val & 0x0f;
        break;
    case 2: uart.fcr = val; break;
    case 3: uart.lcr = val; break;
    case 4: uart.mcr = val & 0x1f; break;
    case 7: uart.scr = val; break;
    }
    uart_update_irq(init);
}

static uint8_t uart_read(uint8_t reg, struct init_struct *init) {
    uint8_t val;
    switch (reg) {
    case 0:
        if (uart.lcr & 0x80) { val = uart.dll; break; }
        if (uart.rx_count) {
            val = uart.rx_fifo[uart.rx_tail];
            uart.rx_tail = (uart.rx_tail + 1) % UART_RX_FIFO_SIZE;
            uart.rx_count--;
        } else {
            val = 0;
        }
        uart.lsr &= ~1;
        if (uart.rx_count) uart.lsr |= 1;
        if (uart.mcr & 0x10) uart.lsr &= ~0x60;
        uart_update_irq(init);
        return val;
    case 1:
        val = (uart.lcr & 0x80) ? uart.dlh : uart.ier;
        break;
    case 2:
        if ((uart.ier & 0x04) && (uart.lsr & 0x1e)) { val = 0x06; break; }
        if ((uart.ier & 0x01) && (uart.lsr & 0x01)) { val = 0x04; break; }
        if ((uart.ier & 0x02) && !uart.thre_ack) { uart.thre_ack = 1; val = 0x02; break; }
        if ((uart.ier & 0x08) && (uart.msr & 0x0f)) { val = 0x00; break; }
        val = 0x01;
        break;
    case 3: val = uart.lcr; break;
    case 4: val = uart.mcr; break;
    case 5:
        val = uart.lsr | 0x60;
        uart.lsr &= 0xe1;
        uart_update_irq(init);
        return val;
    case 6:
        val = uart.msr;
        uart.msr &= 0xf0;
        uart_update_irq(init);
        return val;
    case 7: val = uart.scr; break;
    default: val = 0; break;
    }
    return val;
}

static void uart_rx_feed(struct init_struct *init) {
    struct pollfd pfd = { .fd = 0, .events = POLLIN };
    while (poll(&pfd, 1, 0) > 0) {
        uint8_t buf[64];
        int n = read(0, buf, sizeof(buf));
        if (n <= 0) break;
        for (int j = 0; j < n; j++) {
            if (uart.rx_count < UART_RX_FIFO_SIZE) {
                uart.rx_fifo[uart.rx_head] = buf[j];
                uart.rx_head = (uart.rx_head + 1) % UART_RX_FIFO_SIZE;
                uart.rx_count++;
            }
        }
    }
    if (uart.rx_count) uart.lsr |= 1;
    uart_update_irq(init);
}

static struct virtio_mmio_dev *virtio_mmio_devs[MAX_VIRTIO_MMIO_DEVS];
static int virtio_mmio_dev_count;
static uint32_t plic_pending;

static void register_virtio_mmio_dev(struct virtio_mmio_dev *dev) {
    if (!dev || virtio_mmio_dev_count >= MAX_VIRTIO_MMIO_DEVS)
        return;
    virtio_mmio_devs[virtio_mmio_dev_count++] = dev;
}

static uint32_t mmio_read_u32(uint8_t *data, uint32_t len) {
    uint32_t val = 0;
    for (uint32_t i = 0; i < len && i < sizeof(val); i++)
        val |= ((uint32_t)data[i]) << (i * 8);
    return val;
}

static void mmio_write_u32(uint8_t *data, uint32_t len, uint32_t val) {
    memset(data, 0, len);
    for (uint32_t i = 0; i < len && i < sizeof(val); i++)
        data[i] = (val >> (i * 8)) & 0xff;
}

static int is_plic_claim_complete(uint64_t addr) {
    return addr >= 0x0c200000 && addr < 0x0c400000 &&
           ((addr - 0x0c200000) & 0xfff) == 4;
}

static void plic_set_irq(struct init_struct *init, uint32_t irq, int level) {
    (void)irq;
    struct kvm_interrupt interrupt = {
        .irq = level ? KVM_INTERRUPT_SET : KVM_INTERRUPT_UNSET,
    };
    if (ioctl(init->vcpu_fd, KVM_INTERRUPT, &interrupt) < 0)
        perror(level ? "[VMM] KVM_INTERRUPT SET failed" :
                       "[VMM] KVM_INTERRUPT UNSET failed");
}

static void handle_mmio(struct kvm_run *run, struct init_struct *init) {
    uint64_t addr = run->mmio.phys_addr;

    if (addr >= 0x10000000 && addr < 0x10000100) {
        uint8_t reg = addr - 0x10000000;
        if (reg > 7) return;
        if (run->mmio.is_write) {
            uart_write(reg, run->mmio.data[0], init);
        } else {
            uint8_t val = uart_read(reg, init);
            memset(run->mmio.data, 0, run->mmio.len);
            run->mmio.data[0] = val;
        }
        return;
    }

    for (int i = 0; i < virtio_mmio_dev_count; i++) {
        if (virtio_mmio_handle_access(virtio_mmio_devs[i], addr,
                                      run->mmio.data, run->mmio.len,
                                      run->mmio.is_write))
            return;
    }

    if (addr >= 0x0c000000 && addr < 0x0c200000) {
        if (run->mmio.len > 8) run->mmio.len = 8;
        if (!run->mmio.is_write) {
            memset(run->mmio.data, 0, run->mmio.len);
        }
        return;
    }

    if (addr >= 0x0c200000 && addr < 0x0c400000) {
        if (run->mmio.len > 8) run->mmio.len = 8;
        if (!is_plic_claim_complete(addr)) {
            if (!run->mmio.is_write)
                memset(run->mmio.data, 0, run->mmio.len);
            return;
        }

        if (run->mmio.is_write) {
            uint32_t irq = mmio_read_u32(run->mmio.data, run->mmio.len);
            if (irq < 32)
                plic_pending &= ~(1U << irq);
            if (!plic_pending)
                plic_set_irq(init, irq, 0);
            // printf("[VMM] plic complete: addr=0x%lx irq=%u pending=0x%x\n",
            //        addr, irq, plic_pending);
            fflush(stdout);
        } else {
            uint32_t irq = 0;
            if (plic_pending) {
                irq = __builtin_ctz(plic_pending);
                plic_pending &= ~(1U << irq);
            }
            mmio_write_u32(run->mmio.data, run->mmio.len, irq);
            if (!plic_pending)
                plic_set_irq(init, irq, 0);
            // printf("[VMM] plic claim: addr=0x%lx irq=%u pending=0x%x\n",
            //        addr, irq, plic_pending);
            fflush(stdout);
        }
        return;
    }
}

static void run_vcpu(int vcpu_fd, struct kvm_run *run, struct init_struct *init) {
    while (1) {
        uart_rx_feed(init);

        if (ioctl(vcpu_fd, KVM_RUN, 0) < 0) {
            printf("KVM_RUN failed\n");
            return;
        }

        switch (run->exit_reason) {
            case KVM_EXIT_MMIO:
                handle_mmio(run, init);
                break;
            case KVM_EXIT_SHUTDOWN:
                printf("\n[VMM] 虚拟机执行完毕，请求关机。\n");
                return;
            case KVM_EXIT_INTERNAL_ERROR:
                printf("\n[VMM] 发生内部错误，退出。\n");
                return;
            default:
                printf("\n[VMM] 未处理的 VM Exit，原因编号: %d\n", run->exit_reason);
                return;
        }
    }
}

int main(int argc, char *argv[]) {
    char *uboot_file = NULL;
    char *dtb_file = NULL;
    char *disk_file = NULL;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--uboot") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing filename after --uboot\n");
                return 1;
            }
            uboot_file = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--dtb") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing filename after --dtb\n");
                return 1;
            }
            dtb_file = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--disk") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing filename after --disk\n");
                return 1;
            }
            disk_file = argv[i + 1];
            i += 2;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!uboot_file || !dtb_file) {
        fprintf(stderr, "USAGE: %s --uboot <file> --dtb <file> [--disk <image>]\n", argv[0]);
        return 1;
    }

    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);

    memset(&uart, 0, sizeof(uart));
    uart.lsr = 0x60;
    uart.msr = 0xf0;
    term_raw();

    struct init_struct init_args = create_vm();
    struct kvm_run *run = init_vcpu_control_memory(init_args.kvm_fd, init_args.vcpu_fd);

    load_guest_binary(&init_args, dtb_file, 0x80000000);
    load_guest_binary(&init_args, uboot_file, 0x80200000);

    init_vcpu_regs(init_args.vcpu_fd, 0x80200000, 0x80000000);

    if (disk_file) {
        struct virtio_blk_device *blk = virtio_blk_init(disk_file);
        if (blk) {
            uint32_t host_features[2] = {
                VIRTIO_BLK_F_BLK_SIZE | VIRTIO_BLK_F_FLUSH,
                1U << (VIRTIO_F_VERSION_1 - 32),
            };
            struct virtio_mmio_dev *blk_mmio = virtio_mmio_init(
                VIRTIO_BLK_MMIO_BASE,
                (void *)(uintptr_t)init_args.mem,
                0x80000000,
                init_args.vcpu_fd,
                2,
                0,
                VIRTIO_BLK_IRQ,
                host_features,
                128,
                1,
                virtio_blk_get_config(blk),
                sizeof(struct virtio_blk_config));
            if (blk_mmio) {
                virtio_mmio_set_plic_pending(blk_mmio, &plic_pending);
                virtio_blk_bind_mmio(blk, blk_mmio);
                register_virtio_mmio_dev(blk_mmio);
            }
        }
    }

    struct virtio_gpu_device *gpu = virtio_gpu_init(VIRTIO_GPU_WIDTH,
                                                    VIRTIO_GPU_HEIGHT);
    if (gpu) {
        uint32_t gpu_features[2] = { 0, 0 };
        struct virtio_mmio_dev *gpu_mmio = virtio_mmio_init(
            VIRTIO_GPU_MMIO_BASE,
            (void *)(uintptr_t)init_args.mem,
            0x80000000,
            init_args.vcpu_fd,
            VIRTIO_GPU_DEVICE_ID,
            0,
            VIRTIO_GPU_IRQ,
            gpu_features,
            128,
            VIRTIO_GPU_NUM_QUEUES,
            virtio_gpu_get_config(gpu),
            sizeof(struct virtio_gpu_config));
        if (gpu_mmio) {
            virtio_mmio_set_plic_pending(gpu_mmio, &plic_pending);
            virtio_gpu_bind_mmio(gpu, gpu_mmio);
            register_virtio_mmio_dev(gpu_mmio);
            printf("[VMM] virtio-gpu enabled: %ux%u base=0x%x irq=%d\n",
                   VIRTIO_GPU_WIDTH, VIRTIO_GPU_HEIGHT,
                   VIRTIO_GPU_MMIO_BASE, VIRTIO_GPU_IRQ);
            fflush(stdout);
        }
    }

    run_vcpu(init_args.vcpu_fd, run, &init_args);

    close(init_args.kvm_fd);
    return 0;
}
