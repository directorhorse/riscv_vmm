# RISC-V KVM VMM

这是一个教学/实验性质的 RISC-V 虚拟机监控器（VMM）。项目通过 Linux KVM 接口创建一个内层 RISC-V 虚拟机，加载 U-Boot 和设备树，并为 guest 提供最小可用的串口、PLIC 和 virtio-mmio 块设备模拟。

典型运行环境是：

```text
x86 host
  -> QEMU RISC-V Linux
      -> 本项目 VMM
          -> U-Boot
              -> Linux kernel
```

也就是说，本项目的 `output/vmm` 需要运行在支持 RISC-V KVM 的 Linux 环境中。仓库中的 `Makefile` 提供了一个外层 QEMU RISC-V Linux 的启动方式，方便在 x86 主机上进行嵌套虚拟化实验。x86主机可以是Linux，也可以是Windows上的WSL。

## 功能

- 使用 `/dev/kvm` 创建 RISC-V VM 和单个 vCPU。
- 映射 1 GiB guest RAM，起始 guest 物理地址为 `0x80000000`。
- 将 DTB 加载到 `0x80000000`。
- 将 U-Boot 加载到 `0x80200000`，并从该地址启动 vCPU。
- 模拟 `ns16550a` 串口，地址为 `0x10000000`。
- 模拟简化版 PLIC，用于给 guest 投递外部中断。
- 模拟 modern virtio-mmio block 设备，地址为 `0x10001000`，中断号为 `2`。
- 支持 virtio-blk 的读、写和 flush 请求，后端为宿主文件形式的磁盘镜像。

## 目录结构

```text
.
├── Makefile
├── vmm.dts
├── disk.img
├── include/
│   ├── kvm_helpers.h
│   ├── virtio_blk.h
│   ├── virtio_mmio.h
│   └── vmm.h
└── src/
    ├── main.c
    ├── virtio_blk.c
    ├── virtio_mmio.c
    └── vmm.c
```

主要文件说明：

- `src/vmm.c`：KVM VM/vCPU 创建、guest 内存映射、镜像加载、寄存器初始化。
- `src/main.c`：命令行参数解析、主运行循环、MMIO exit 分发、UART 和 PLIC 模拟。
- `src/virtio_mmio.c`：modern virtio-mmio 寄存器和 virtqueue 处理。
- `src/virtio_blk.c`：virtio-blk 后端，实现对磁盘镜像的 `pread`、`pwrite` 和 `fsync`。
- `vmm.dts`：内层 guest 使用的设备树。
- `Makefile`：编译 VMM、生成 DTB、启动外层 QEMU 环境。

## 依赖

主机侧需要：

- `qemu-system-riscv64，推荐9.0版本及以上`
- `RISC-V GCC工具链`
- `dtc`
- `make`
- 一个可启动的外层 RISC-V Linux 磁盘镜像

外层 RISC-V Linux 需要：

- 可访问 `/dev/kvm`
- 支持 RISC-V KVM
- 能运行交叉编译生成的 `output/vmm`

## 构建

编译 VMM：

```sh
make vmm
```

输出文件：

```text
output/vmm
```

生成内层 guest 使用的 DTB：

```sh
make dtb
```

输出文件：

```text
output/vmm.dtb
```

清理构建产物：

```sh
make clean
```

## 启动外层 RISC-V Linux

`Makefile` 中的 `run` 目标会启动一个外层 QEMU RISC-V Linux：

```sh
make run
```

后台启动：

```sh
make start
```

停止后台 QEMU：

```sh
make stop
```

默认 QEMU 配置使用：

```text
-machine virt,aia=aplic-imsic,aia-guests=4
-cpu rv64,h=true
-m 8G
-smp 4
```

这表示外层 QEMU 会启用 RISC-V H 扩展和 AIA 相关配置，以便外层 Linux 可以提供 KVM 给本项目使用。

默认磁盘镜像路径为：

```text
qemu_config/my-vmm-workspace.qcow2
```

如果你的镜像路径不同，需要修改 `Makefile` 中的 `IMG` 变量。

## 传输 VMM 到外层 Linux

外层 QEMU 默认开启 SSH 端口转发：

```text
host 2222 -> guest 22
```

可以使用：

```sh
make ssh
```

将构建结果传到外层 Linux：

```sh
make trans
```

该命令会把 `output/` 下的文件复制到外层 Linux 的用户 home 目录。

## 运行内层虚拟机

在外层 RISC-V Linux 中运行：

```sh
./vmm --uboot <u-boot.bin> --dtb <vmm.dtb> --disk <disk.img>
```

示例：

```sh
./vmm --uboot u-boot.bin --dtb vmm.dtb --disk disk.img
```

参数说明：

- `--uboot <file>`：内层 guest 的 U-Boot 镜像，加载到 `0x80200000`。
- `--dtb <file>`：内层 guest 的设备树 blob，加载到 `0x80000000`。
- `--disk <image>`：virtio-blk 后端磁盘镜像，可选，但启动 Linux 根文件系统通常需要。

启动后，VMM 会进入 `KVM_RUN` 循环，并处理内层 guest 产生的 MMIO exit。串口输出会直接显示在运行 VMM 的终端中，终端输入也会通过模拟 UART 发送给 guest。

## U-Boot 启动 Linux 示例

如果磁盘镜像中包含 Linux `Image` 和 `vmm.dtb`，可在 U-Boot 中执行类似命令：

```sh
load virtio 0:1 ${kernel_addr_r} Image
load virtio 0:1 ${fdt_addr_r} vmm.dtb
setenv bootargs "earlycon console=ttyS0,115200n8 rootwait rw root=/dev/vda2"
booti ${kernel_addr_r} - ${fdt_addr_r}
```

具体分区号和文件路径需要根据你的磁盘镜像调整。

## 虚拟硬件布局

内层 guest 的主要地址布局：

| 设备 | 地址 | 说明 |
| --- | --- | --- |
| RAM | `0x80000000` | 1 GiB guest memory |
| DTB | `0x80000000` | VMM 加载位置 |
| U-Boot | `0x80200000` | vCPU 入口地址 |
| UART | `0x10000000` | `ns16550a` |
| virtio-mmio blk | `0x10001000` | virtio block device |
| PLIC | `0x0c000000` | 简化 PLIC 模拟 |

virtio-blk 的中断号为 `2`，需要和 `vmm.dts` 中的 `interrupts = <2>;` 保持一致。

## 调试日志

VMM 会打印 virtio-mmio 和 virtio-blk 的关键日志，例如：

```text
[VMM] virtio mmio write: off=0x50 val=0x0 len=4
[VMM] virtio-blk notify: num_new=1 avail_idx=0 ring_size=128
[VMM] virtio-blk request: desc=0 type=0 sector=0 len=4096 used_len=4097 status=0
[VMM] virtio irq inject: plic_irq=2 interrupt_status=0x1
[VMM] plic claim: addr=0xc201004 irq=2 pending=0x0
[VMM] plic complete: addr=0xc201004 irq=2 pending=0x0
```

这些日志可以帮助判断问题发生在：

- guest 是否写了 `QUEUE_NOTIFY`
- VMM 是否看到了新的 virtqueue 请求
- virtio-blk 后端是否成功读写磁盘
- virtio 中断是否注入
- Linux 是否进入 PLIC claim/complete

## 已知限制

- 只创建一个 vCPU。
- PLIC 是简化实现，并不完整模拟所有寄存器语义。
- virtio-mmio/virtio-blk 只实现了当前启动 Linux 所需的基础路径。
- virtio-blk 不支持 discard、write zeroes 等高级请求。
- 当前项目偏实验用途，不是完整通用 VMM。

## 常见问题

### 看不到 `/dev/kvm`

说明外层 RISC-V Linux 没有启用 KVM，或 QEMU 没有提供 H 扩展/相关虚拟化能力。需要检查外层 QEMU 参数和外层 Linux 内核配置。可以尝试sudo modprobe kvm，如果没有效果，可能是QEMU版本或RISC-V Linux镜像存在问题。
