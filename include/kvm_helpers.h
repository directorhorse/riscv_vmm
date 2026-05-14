#ifndef KVM_HELPERS_H
#define KVM_HELPERS_H

#include <stdint.h>

int kvm_set_reg(int vcpu_fd, uint64_t reg_id, uint64_t value);
int kvm_get_reg(int vcpu_fd, uint64_t reg_id, uint64_t *value);

#endif
