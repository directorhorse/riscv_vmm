GNU = riscv64-unknown-linux-gnu
CC = $(GNU)-gcc
CFLAGS = -g -Wall -Iinclude
BUILD = build
SRC = src
OUTPUT = output
OBJ = $(BUILD)/vmm.o $(BUILD)/main.o $(BUILD)/virtio_mmio.o $(BUILD)/virtio_blk.o

QEMU = qemu-system-riscv64
QEMU_CONFIG = qemu_config
IMG = $(QEMU_CONFIG)/my-vmm-workspace.qcow2

QEMU_FLAGS = \
	-machine virt,aia=aplic-imsic,aia-guests=4 \
	-cpu rv64,h=true \
	-m 8G \
	-smp 4 \
	-kernel /usr/lib/u-boot/qemu-riscv64_smode/u-boot.bin \
	-netdev user,id=net0,hostfwd=tcp::2222-:22 \
	-device virtio-net-device,netdev=net0 \
	-drive file=$(IMG),format=qcow2,if=virtio \
	-append "root=/dev/vda1 rw earlycon=sbi console=ttyS0"

DAEMON_OPTS = -daemonize -display none -serial file:$(QEMU_CONFIG)/qemu.log -pidfile $(QEMU_CONFIG)/qemu.pid

.PHONY: all clean run start ssh trans stop vmm dtb

all: vmm

vmm: $(OBJ)
	@mkdir -p $(OUTPUT)
	$(CC) $(OBJ) -o $(OUTPUT)/vmm

$(BUILD)/vmm.o: $(SRC)/vmm.c include/vmm.h include/kvm_helpers.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $(SRC)/vmm.c -o $@

$(BUILD)/main.o: $(SRC)/main.c include/vmm.h include/virtio_mmio.h include/virtio_blk.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $(SRC)/main.c -o $@

$(BUILD)/virtio_mmio.o: $(SRC)/virtio_mmio.c include/virtio_mmio.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $(SRC)/virtio_mmio.c -o $@

$(BUILD)/virtio_blk.o: $(SRC)/virtio_blk.c include/virtio_blk.h include/virtio_mmio.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $(SRC)/virtio_blk.c -o $@

dtb:
	@mkdir -p $(OUTPUT)
	dtc -I dts -O dtb -o $(OUTPUT)/vmm.dtb vmm.dts

clean:
	rm -rf $(BUILD) $(OUTPUT)/vmm $(OUTPUT)/vmm.dtb
run:
	$(QEMU) $(QEMU_FLAGS) -nographic

start:
	$(QEMU) $(QEMU_FLAGS) $(DAEMON_OPTS)

ssh:
	ssh -p 2222 ubuntu@127.0.0.1

trans: vmm
	scp -P 2222 $(OUTPUT)/* ubuntu@127.0.0.1:~

stop:
	@if [ -f $(QEMU_CONFIG)/qemu.pid ]; then \
		kill -9 `cat $(QEMU_CONFIG)/qemu.pid` && rm $(QEMU_CONFIG)/qemu.pid; \
		echo "虚拟机已强制关闭。"; \
	else \
		echo "未找到 qemu.pid，虚拟机似乎没有在后台运行。"; \
	fi
