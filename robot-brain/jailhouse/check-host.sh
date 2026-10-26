#!/bin/bash
# check-host.sh — 这台机器能不能跑 Jailhouse 的大小脑
#
# 不猜，只查。每一项都给出实际值，最后给 GO / NO-GO。
# 在候选机器上直接跑：bash jailhouse/check-host.sh
#
# 为什么需要这个脚本：
#   Jailhouse 不是装个软件就能用的东西，它对硬件有硬性要求。很多人卡在
#   "insmod 之后没反应"，其实第一步就不满足。先跑这个，省半天。

set -u
fail=0
warn=0

say()  { printf "  %-34s %s\n" "$1" "$2"; }
ok()   { printf "  \033[32m[OK]\033[0m   %-27s %s\n" "$1" "$2"; }
no()   { printf "  \033[31m[NO]\033[0m   %-27s %s\n" "$1" "$2"; fail=$((fail+1)); }
warn() { printf "  \033[33m[!]\033[0m    %-27s %s\n" "$1" "$2"; warn=$((warn+1)); }

echo
echo "==============================================================="
echo " Jailhouse 主机就绪检查"
echo "==============================================================="
echo

arch=$(uname -m)
say "架构" "$arch"
say "内核" "$(uname -r)"
say "逻辑核数" "$(nproc)"
say "内存" "$(free -g | awk '/^Mem:/{print $2" GB"}')"
echo

# —— 1. 硬件虚拟化扩展 ——
echo "  1) 硬件虚拟化扩展"
if [ "$arch" = "x86_64" ]; then
    if grep -q -m1 -w vmx /proc/cpuinfo; then
        ok "Intel VT-x" "vmx 已暴露"
    elif grep -q -m1 -w svm /proc/cpuinfo; then
        ok "AMD-V" "svm 已暴露"
    else
        no "vmx / svm" "都没有 —— Jailhouse 无法初始化"
    fi
elif [ "$arch" = "aarch64" ]; then
    if dmesg 2>/dev/null | grep -qi "EL2"; then
        ok "ARM EL2" "内核运行在 EL2 可用状态"
    else
        warn "ARM EL2" "dmesg 里没看到 EL2，需确认板子支持虚拟化扩展"
    fi
else
    no "架构" "$arch 不在 Jailhouse 支持范围"
fi
echo

# —— 2. 是不是别人的 guest ——
echo "  2) 虚拟化嵌套状态"
if grep -q -m1 hypervisor /proc/cpuinfo; then
    warn "hypervisor 标志" "这台机器自己就是 guest"
    echo "       如果是 VMware：关机 → 设置 → 处理器 → 勾选「虚拟化 Intel VT-x/EPT」"
    echo "       如果是 KVM：宿主要开 nested=1"
else
    ok "hypervisor 标志" "看起来是物理机"
fi
echo

# —— 3. 核数够不够 ——
echo "  3) 核数预算"
cores=$(nproc)
if [ "$cores" -ge 4 ]; then
    ok "可用核数" "$cores 个（root cell 2 + inmate 1，够）"
elif [ "$cores" -ge 2 ]; then
    warn "可用核数" "$cores 个（够分，但没有余量，时序数据会很差）"
else
    no "可用核数" "$cores 个（至少 2 个：一个给 Linux，一个给 RTOS）"
fi
echo

# —— 4. 内存预算 ——
echo "  4) 内存预算"
mem_mb=$(free -m | awk '/^Mem:/{print $2}')
if [ "$mem_mb" -ge 2048 ]; then
    ok "内存" "${mem_mb} MB"
else
    no "内存" "${mem_mb} MB（inmate 至少需要 16 MB 独占，root cell 要够跑 openEuler）"
fi
echo

# —— 5. 构建依赖 ——
echo "  5) 构建依赖"
for t in gcc make git python3; do
    if command -v "$t" >/dev/null 2>&1; then
        ok "$t" "$(command -v $t)"
    else
        no "$t" "未安装"
    fi
done
if [ -d "/lib/modules/$(uname -r)/build" ]; then
    ok "内核头文件" "/lib/modules/$(uname -r)/build"
else
    no "内核头文件" "缺 build 目录，Jailhouse 的内核模块编不了"
fi
echo

# —— 6. 现有 Jailhouse ——
echo "  6) 已有安装"
if command -v jailhouse >/dev/null 2>&1; then
    ok "jailhouse 命令" "$(command -v jailhouse)"
else
    warn "jailhouse 命令" "未安装，需要从源码编"
fi
if lsmod 2>/dev/null | grep -q '^jailhouse'; then
    ok "jailhouse 模块" "已加载"
else
    warn "jailhouse 模块" "未加载"
fi
echo

# —— 结论 ——
echo "==============================================================="
if [ "$fail" -eq 0 ]; then
    echo " 结论：GO —— 这台机器可以尝试部署"
    echo
    echo " 下一步：sudo bash jailhouse/deploy.sh"
else
    echo " 结论：NO-GO —— 有 $fail 项硬性条件不满足"
    echo
    echo " 不满足的项在上面标了 [NO]。这些不是装软件能解决的："
    echo "   - 没有 vmx/svm  → 换物理机，或让宿主打开嵌套虚拟化"
    echo "   - 核数不足      → 换核数更多的机器"
    echo "   - 内存不足      → 加内存"
    echo
    echo " 如果这台是 VMware/KVM 虚拟机，先按上面第 2 项提示打开嵌套虚拟化，"
    echo " 彻底关机再开机，然后重跑本脚本。"
fi
echo "==============================================================="
echo
exit "$fail"
