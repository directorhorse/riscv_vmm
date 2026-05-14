#include "vmm.h"
#include "kvm_helpers.h"
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
#include <stddef.h>

int kvm_set_reg(int vcpu_fd, uint64_t reg_id, uint64_t value) {
    struct kvm_one_reg reg;
    reg.id = reg_id;
    reg.addr = (uint64_t)&value;
    return ioctl(vcpu_fd, KVM_SET_ONE_REG, &reg);
}

int kvm_get_reg(int vcpu_fd, uint64_t reg_id, uint64_t *value) {
    struct kvm_one_reg reg;
    reg.id = reg_id;
    reg.addr = (uint64_t)value;
    return ioctl(vcpu_fd, KVM_GET_ONE_REG, &reg);
}

int set_riscv_reg(int vcpu_fd, uint64_t reg_id, uint64_t value) {
    return kvm_set_reg(vcpu_fd, reg_id, value);
}

static void *load_image(const char *filename, uint64_t mem, size_t *size_out) {
    int img_fd = open(filename, O_RDONLY);
    if (img_fd < 0) {
        fprintf(stderr, "can not open binary file: %d\n", errno);
        return NULL;
    }

    char *p = (char *)mem;
    size_t total_read = 0;

    for (;;) {
        int r = read(img_fd, p, 4096);
        if (r < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[VMM 错误] 读取文件时发生错误: %d\n", errno);
            close(img_fd);
            return NULL;
        }
        if (r == 0) break;
        p += r;
        total_read += r;
    }

    close(img_fd);
    __builtin___clear_cache((char *)mem, (char *)mem + total_read);

    if (size_out) *size_out = total_read;
    return (void *)mem;
}

struct init_struct create_vm(void) {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    void *mem;

    if ((kvm_fd = open("/dev/kvm", O_RDWR)) < 0) {
        fprintf(stderr, "failed to open /dev/kvm: %d\n", errno);
        exit(-1);
    }

    if ((vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0)) < 0) {
        fprintf(stderr, "failed to create vm: %d\n", errno);
        exit(-1);
    }

    mem = mmap(NULL, 1 << 30, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == NULL) {
        fprintf(stderr, "mmap failed: %d\n", errno);
        exit(-1);
    }

    struct kvm_userspace_memory_region region;
    memset(&region, 0, sizeof(region));
    region.slot = 0;
    region.guest_phys_addr = 0x80000000;
    region.memory_size = 1 << 30;
    region.userspace_addr = (uintptr_t)mem;
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        fprintf(stderr, "ioctl KVM_SET_USER_MEMORY_REGION failed: %d\n", errno);
        exit(-1);
    }

    if ((vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0)) < 0) {
        fprintf(stderr, "can not create vcpu: %d\n", errno);
        exit(-1);
    }

    struct init_struct retval = {
        kvm_fd, vm_fd, vcpu_fd, (uint64_t)mem
    };
    return retval;
}

int load_guest_binary(struct init_struct *init, const char *filename, uint64_t guest_addr) {
    uint64_t offset = guest_addr - 0x80000000;
    uint64_t host_addr = init->mem + offset;
    size_t img_size = 0;
    if (load_image(filename, host_addr, &img_size) == NULL) {
        return -1;
    }
    printf("[VMM] 成功加载镜像 '%s'，共读取 %zu 字节到地址 0x%lx\n",
           filename, img_size, guest_addr);
    return 0;
}

struct kvm_run *init_vcpu_control_memory(int kvm_fd, int vcpu_fd) {
    int kvm_run_mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (kvm_run_mmap_size < 0) {
        fprintf(stderr, "ioctl KVM_GET_VCPU_MMAP_SIZE: %d\n", errno);
        exit(-1);
    }

    struct kvm_run *run = mmap(NULL, kvm_run_mmap_size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, vcpu_fd, 0);
    if (run == NULL) {
        fprintf(stderr, "mmap kvm_run: %d\n", errno);
        exit(-1);
    }
    return run;
}

void init_vcpu_regs(int vcpu_fd, uint64_t entry_addr, uint64_t dtb_addr) {
    uint64_t core_reg_base = KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_CORE;

    uint64_t pc_id = core_reg_base | KVM_REG_RISCV_CORE_REG(regs.pc);
    set_riscv_reg(vcpu_fd, pc_id, entry_addr);

    uint64_t a0_id = core_reg_base | KVM_REG_RISCV_CORE_REG(regs.a0);
    set_riscv_reg(vcpu_fd, a0_id, 0);

    uint64_t a1_id = core_reg_base | KVM_REG_RISCV_CORE_REG(regs.a1);
    set_riscv_reg(vcpu_fd, a1_id, dtb_addr);

    printf("Powering on RISC-V vCPU...\n");
    struct kvm_mp_state mp_state;
    mp_state.mp_state = KVM_MP_STATE_RUNNABLE;
    if (ioctl(vcpu_fd, KVM_SET_MP_STATE, &mp_state) < 0) {
        fprintf(stderr, "KVM_SET_MP_STATE 唤醒 vCPU 失败: %d\n", errno);
        exit(-1);
    }

    uint64_t csr_reg_base = KVM_REG_RISCV | KVM_REG_SIZE_U64 | KVM_REG_RISCV_CSR;
    uint64_t satp_id = csr_reg_base | KVM_REG_RISCV_CSR_REG(satp);
    set_riscv_reg(vcpu_fd, satp_id, 0);
}
