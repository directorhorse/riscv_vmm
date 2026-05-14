#ifndef VMM_H
#define VMM_H

#include <stdint.h>

struct kvm_run;

struct init_struct {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    uint64_t mem;
};

int set_riscv_reg(int vcpu_fd, uint64_t reg_id, uint64_t value);
struct init_struct create_vm(void);
int load_guest_binary(struct init_struct *init, const char *filename, uint64_t guest_addr);
struct kvm_run *init_vcpu_control_memory(int kvm_fd, int vcpu_fd);
void init_vcpu_regs(int vcpu_fd, uint64_t entry_addr, uint64_t dtb_addr);

#endif
