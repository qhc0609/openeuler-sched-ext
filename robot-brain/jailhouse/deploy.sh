#!/bin/bash
# deploy.sh — 在真机上把大小脑按 Jailhouse 的双 cell 布起来
#
# 这个脚本做的事，就是"多系统混合部署"的完整动作序列：
#   1. 把 Linux 降级成 root cell（大脑住这里）
#   2. 建一个 ivshmem 设备，作为两个系统之间唯一的共享内存
#   3. 建第二个 cell，把 RTOS（小脑）放进去，给它独占的核和内存
#   4. 两个系统各自跑各自的，只通过那 64 KB 通信
#
# 用法：
#   sudo bash jailhouse/deploy.sh check      # 只做就绪检查
#   sudo bash jailhouse/deploy.sh build      # 取源码并编译
#   sudo bash jailhouse/deploy.sh rootcell   # 生成 root cell 配置
#   sudo bash jailhouse/deploy.sh up         # 建 cell 并启动小脑
#   sudo bash jailhouse/deploy.sh down       # 拆掉，回到普通 Linux
#   sudo bash jailhouse/deploy.sh brain      # 在 root cell 里跑大脑
#
# 【状态】除了 check 和 build，其余步骤都没有在本仓库的开发环境里跑过，
# 因为那台 VMware 虚拟机没有 vmx。详见 README.md。

set -eu

JH_SRC=${JH_SRC:-/opt/jailhouse}
JH_WORK=${JH_WORK:-/var/lib/jailhouse}
ROOTCELL=${ROOTCELL:-$JH_WORK/rootcell.cell}
ROBOTCELL=${ROBOTCELL:-$JH_WORK/robot.cell}
INMATE_BIN=${INMATE_BIN:-/root/robot-brain/bin/cerebellum-inmate.bin}
HERE=$(cd "$(dirname "$0")" && pwd)
BRAIN_DIR=$(cd "$HERE/.." && pwd)

log()  { printf "\033[36m==>\033[0m %s\n" "$*"; }
die()  { printf "\033[31m错误:\033[0m %s\n" "$*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || die "需要 root"
}

cmd_check() {
    bash "$HERE/check-host.sh" || die "主机就绪检查未通过，先解决上面标 [NO] 的项"
}

cmd_build() {
    need_root
    log "取 Jailhouse 源码到 $JH_SRC"
    if [ ! -d "$JH_SRC/.git" ]; then
        git clone https://github.com/siemens/jailhouse "$JH_SRC" \
            || die "clone 失败。如果这台机器连不上 github，把源码用别的方式拷到 $JH_SRC 再重跑"
    else
        log "源码已存在，跳过 clone"
    fi

    log "编译 hypervisor、工具和内核模块"
    make -C "$JH_SRC" -j"$(nproc)" || die "编译失败，先看缺什么依赖"

    log "编译完成，产物："
    ls -1 "$JH_SRC/hypervisor/jailhouse.ko" "$JH_SRC/tools/jailhouse" 2>/dev/null || true
}

cmd_rootcell() {
    need_root
    [ -f "$JH_SRC/tools/jailhouse" ] || die "还没编译，先跑：$0 build"
    mkdir -p "$JH_WORK"

    log "加载 hypervisor 模块"
    if lsmod | grep -q '^jailhouse'; then
        log "模块已加载"
    else
        insmod "$JH_SRC/hypervisor/jailhouse.ko" || die "insmod 失败，多半是 vmx 没暴露"
    fi

    log "硬件自检（这一步会告诉你哪些资源可用）"
    "$JH_SRC/tools/jailhouse" hardware check || die "硬件自检未通过"

    log "生成 root cell 配置"
    cd "$JH_SRC"
    ./tools/jailhouse config create -o "$ROOTCELL" rootcell.c \
        || die "生成 root cell 配置失败"
    log "root cell 配置：$ROOTCELL"
    log "接下来要把这个文件里的内存区、PCI BDF、GIC 地址抄进 robot-cell.c"
}

cmd_up() {
    need_root
    [ -f "$ROOTCELL" ] || die "root cell 配置不存在，先跑：$0 rootcell"
    [ -f "$INMATE_BIN" ] || die "小脑镜像不存在：$INMATE_BIN（先编 inmate，见 inmate/README.md）"

    log "把 Linux 降级成 root cell"
    "$JH_SRC/tools/jailhouse" enable "$ROOTCELL"

    log "创建 ivshmem 设备（大小脑唯一的共享内存）"
    # 参数：shmem-region0 的名字和大小，以及给对端的 MSI 向量数
    "$JH_SRC/tools/jailhouse" config ivshmem-create robot-shm --size 0x10000 \
        --peer robot-cerebellum --vectors 2

    log "在 root cell 里找到 ivshmem 的 BDF"
    lspci -nn | grep -i '1af4:1110' || \
        log "没找到？把 lspci 输出发出来，可能需要手动指定 BDF"

    log "创建小脑 cell"
    "$JH_SRC/tools/jailhouse" cell create "$ROBOTCELL"

    log "把镜像载进小脑 cell"
    "$JH_SRC/tools/jailhouse" cell load robot-cerebellum "$INMATE_BIN"

    log "启动小脑"
    "$JH_SRC/tools/jailhouse" cell start robot-cerebellum

    echo
    log "现在小脑是一个独立的系统，在自己的核上跑自己的控制环。"
    log "下一步在另一个终端跑大脑："
    log "  $0 brain"
}

cmd_brain() {
    need_root
    local bdf
    bdf=$(lspci -D | grep -i '1af4:1110' | awk '{print $1}' | head -1)
    [ -n "$bdf" ] || die "找不到 ivshmem 设备，先跑：$0 up"

    log "ivshmem 设备：$bdf"
    log "启动大脑（它通过 ivshmem 给小脑发轨迹）"
    exec "$BRAIN_DIR/bin/brain" \
        --transport "ivshmem:/sys/bus/pci/devices/$bdf" \
        --hz 20 --duration-s "${1:-0}"
}

cmd_down() {
    need_root
    log "关闭小脑 cell"
    "$JH_SRC/tools/jailhouse" cell shutdown robot-cerebellum 2>/dev/null || true
    log "销毁小脑 cell"
    "$JH_SRC/tools/jailhouse" cell destroy robot-cerebellum 2>/dev/null || true
    log "禁用 hypervisor，Linux 恢复完整控制"
    "$JH_SRC/tools/jailhouse" disable
    log "完成"
}

case "${1:-}" in
    check)    cmd_check ;;
    build)    cmd_build ;;
    rootcell) cmd_rootcell ;;
    up)       cmd_up ;;
    brain)    cmd_brain "${2:-0}" ;;
    down)     cmd_down ;;
    *)
        sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
