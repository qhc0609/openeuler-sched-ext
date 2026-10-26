/*
 * robot-cell.c — 小脑 cell 的配置模板
 *
 * 【这个文件没有验证过，也不能直接编译通过。】
 *
 * 它是一份模板，不是成品。Jailhouse 的 cell 配置和具体板子的物理内存映射
 * 绑死：PCI 设备的 BDF、GIC 的中断号、RAM 的物理地址区间、串口地址，
 * 换一块板子全都要改。没有板子在手，写出来的东西只能是模板。
 *
 * 拿到真机之后怎么填：
 *   1. 先在 root cell 里跑 `jailhouse hardware check`，它会告诉你哪些资源可用
 *   2. 用 `jailhouse config create` 从当前系统生成 root cell 配置
 *   3. 从 root cell 的配置里抄出 mem_regions 的物理地址区间
 *   4. 把下面标了 TODO 的地方逐个替换
 *
 * 编译方式（在 Jailhouse 源码树里）：
 *   make -C tools cell-config
 *   tools/jailhouse-config-create robot-cell.c > robot.cell
 *
 * 加载：
 *   jailhouse enable /path/to/rootcell.cell
 *   jailhouse cell create robot.cell
 *   jailhouse cell load robot-cerebellum /path/to/cerebellum.bin
 *   jailhouse cell start robot-cerebellum
 */
#include <jailhouse/types.h>
#include <jailhouse/cell-config.h>

struct {
    struct jailhouse_cell_desc cell;
    __u64 cpus[1];
    struct jailhouse_memory mem_regions[5];
    struct jailhouse_irqchip irqchips[1];
    struct jailhouse_pci_device pci_devices[1];
} __attribute__((packed)) config = {
    .cell = {
        .name = "robot-cerebellum",
        /*
         * PASSIVE_COMMREG：小脑 cell 不用 hypercall 页，减少一次陷出。
         * 1 kHz 的控制环里每一次陷出都是几十微秒，能省就省。
         */
        .flags = JAILHOUSE_CELL_PASSIVE_COMMREG,
        .cpu_set_size = sizeof(config.cpus),
        .num_memory_regions = ARRAY_SIZE(config.mem_regions),
        .num_irqchips = ARRAY_SIZE(config.irqchips),
        .num_pci_devices = ARRAY_SIZE(config.pci_devices),
        .vpci_irq_base = 0,   /* TODO: 从 root cell 配置里抄 */
    },

    /*
     * CPU 集合：位图。0x2 表示只用 CPU1。
     * 小脑独占一个物理核，这是整套架构能成立的前提。
     * TODO: 按实际板子的核数调整，别和 root cell 重叠。
     */
    .cpus = {
        0x2,
    },

    .mem_regions = {
        /*
         * 1) 小脑自己的代码和数据。
         *    TODO: 物理地址必须是一块 root cell 没有用到的 RAM。
         *    大小按小脑镜像的实际尺寸 + 栈 + 堆来定，留一倍余量。
         */
        {
            .phys_start = 0x3f000000,
            .virt_start = 0x0,
            .size = 0x01000000,          /* 16 MB */
            .flags = JAILHOUSE_MEM_READ | JAILHOUSE_MEM_WRITE |
                     JAILHOUSE_MEM_EXECUTE | JAILHOUSE_MEM_LOADABLE,
        },

        /*
         * 2) 串口，调试用。
         *    TODO: 地址按板子改。树莓派是 0xfe215040，i.MX8M 是 0x30860000。
         */
        {
            .phys_start = 0xfe215040,
            .virt_start = 0xfe215040,
            .size = 0x1000,
            .flags = JAILHOUSE_MEM_READ | JAILHOUSE_MEM_WRITE |
                     JAILHOUSE_MEM_IO,
        },

        /*
         * 3) GIC 分发器。小脑要收中断就必须能访问它。
         *    TODO: 地址按板子改。
         */
        {
            .phys_start = 0xff841000,
            .virt_start = 0xff841000,
            .size = 0x2000,
            .flags = JAILHOUSE_MEM_READ | JAILHOUSE_MEM_WRITE |
                     JAILHOUSE_MEM_IO,
        },

        /*
         * 4) 通信区：这就是大小脑的共享内存。
         *    大小要和 include/shm_proto.h 里的 RB_SHM_SIZE 对齐（64 KB），
         *    并且必须是 4 KB 对齐。
         *
         *    这一块在 root cell 的配置里也要出现一次，两边指向同一段物理地址，
         *    这就是"共享"的全部含义——hypervisor 不做任何转发。
         *
         *    TODO: 换一个 root cell 没占用的物理地址。
         */
        {
            .phys_start = 0x3e000000,
            .virt_start = 0x3e000000,
            .size = 0x00010000,          /* 64 KB */
            .flags = JAILHOUSE_MEM_READ | JAILHOUSE_MEM_WRITE,
        },

        /*
         * 5) 中断控制器（GIC redistributor），收 IPI 用。
         *    TODO: 地址按板子改。
         */
        {
            .phys_start = 0xff842000,
            .virt_start = 0xff842000,
            .size = 0x100000,
            .flags = JAILHOUSE_MEM_READ | JAILHOUSE_MEM_WRITE |
                     JAILHOUSE_MEM_IO,
        },
    },

    .irqchips = {
        /* TODO: GIC 版本和地址按板子改 */
        {
            .address = 0xff841000,
            .pin_base = 32,
            .pin_bitmap = {
                /* 给串口和 ivshmem 中断留位，具体哪一位要看设备树 */
                0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
            },
        },
    },

    .pci_devices = {
        /*
         * ivshmem 虚拟网卡。
         * TODO: type 和 BDF 必须和 root cell 里创建 ivshmem 时一致。
         *       在 root cell 里用 `lspci` 找到它，然后抄过来。
         */
        {
            .type = JAILHOUSE_PCI_TYPE_IVSHMEM,
            .domain = 0,
            .bdf = 0x0008,               /* 00:01.0，示例 */
            .bar_mask = {
                0xfffffff0, 0x0,         /* BAR0：doorbell */
                0xfffffff0, 0x0,         /* BAR1：状态 */
                0xffff0000, 0x0,         /* BAR2：共享内存 */
            },
            .shmem_region = 3,           /* 指向上面 mem_regions[3] */
            .num_msix_vectors = 2,       /* CMD 和 ESTOP 各一个 */
        },
    },
};
