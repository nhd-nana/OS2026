#!/usr/bin/env bash
#
# Lab2 分支任务：用双 GDB "透视" QEMU 的地址翻译
#
#   一个 gdb（x86_64 原生）调试 QEMU 进程本身 —— 看它怎么模拟 MMU
#   一个 gdb（riscv 交叉）通过 gdbstub 调试内核 —— 让 ucore 跑起来
#
# 用法（在本目录下执行）：
#   ./demo-mmu.sh          自动跑完整演示，输出带标注的观察记录
#   ./demo-mmu.sh manual   只打印"三个终端"的手工操作步骤
#
# 前提：
#   - QEMU 4.1.1 带调试信息（~/opt/qemu-4.1.1/bin/qemu-system-riscv64，--enable-debug 编译）
#   - QEMU 源码在 ~/qemu-4.1.1-build/qemu-4.1.1（用于 gdb 显示源码）
#   - 已安装 x86_64 版 gdb（sudo apt install gdb）
set -u

cd "$(dirname "$0")" || exit 1

QEMU_BIN="${QEMU_BIN:-$HOME/opt/qemu-4.1.1/bin/qemu-system-riscv64}"
QEMU_SRC="${QEMU_SRC:-$HOME/qemu-4.1.1-build/qemu-4.1.1}"
RV_GDB="${RV_GDB:-riscv64-unknown-elf-gdb}"
QEMU_ARGS="-machine virt -nographic -bios default -kernel bin/ucore.img -s -S -monitor none"
LOG_DIR="$(mktemp -d /tmp/lab2_mmu.XXXXXX)"
NATIVE_GDB_SCRIPT="$LOG_DIR/native.gdb"
RV_GDB_SCRIPT="$LOG_DIR/rv.gdb"
NATIVE_LOG="$LOG_DIR/native.log"
RV_LOG="$LOG_DIR/rv.log"
SERIAL_LOG="$LOG_DIR/serial.log"

info() { printf '\033[36m[demo]\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m[demo]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[demo]\033[0m %s\n' "$*"; }

cleanup() {
    pkill -x qemu-system-riscv64 2>/dev/null
    if [ "${KEEP_LOG:-0}" = "1" ]; then
        echo "[demo] 日志保留在 $LOG_DIR"
    else
        rm -rf "$LOG_DIR"
    fi
}
trap cleanup EXIT

cmd_manual() {
    cat <<'MANUAL'
三个终端（按顺序）：

终端 1 —— 启动调试版 QEMU，复位即停，等 gdb 连 1234：
    cd <仓库根>/code
    make debug                      # = qemu ... -kernel bin/ucore.img -s -S

终端 2 —— 用 **x86_64 的 gdb** 附加到 QEMU 进程，调试模拟器本身：
    pgrep -f qemu-system-riscv64    # 拿到 PID
    sudo gdb                        # 需要 sudo（ptrace_scope=1 时普通用户不能 attach）
    (gdb) attach <PID>
    (gdb) handle SIGPIPE nostop noprint     # 不设这条，QEMU 的 SIGPIPE 会让 gdb 反复中断
    (gdb) set pagination off
    (gdb) dir ~/qemu-4.1.1-build/qemu-4.1.1  # 让 gdb 找到源码
    (gdb) break get_physical_address if addr == 0xffffffffc02000d6
    (gdb) break /home/ubuntu/qemu-4.1.1-build/qemu-4.1.1/target/riscv/cpu_helper.c:256
    (gdb) continue                          # 之后等终端 3 让内核跑起来

终端 3 —— riscv 交叉 gdb 连上 gdbstub，让内核执行：
    cd <仓库根>/code
    make gdb                                # 或 riscv64-unknown-elf-gdb -ex 'target remote localhost:1234'
    (gdb) continue                          # 内核一开始跑，终端 2 就会命中断点

然后在终端 2 单步/打印，观察 TLB 查询与三级查表；完事在终端 2 `detach`，
终端 3 `quit`，终端 1 里 Ctrl+A 松手再按 X 退出。

关键源码位置（QEMU 4.1.1，target/riscv/cpu_helper.c）：
    155  get_physical_address()       —— 地址翻译总入口（TLB miss 时才调）
    172  裸机/M 态的提前返回           —— *physical = addr，不查表
    184  base = satp.PPN << 12        —— 根表物理地址
    196  case VM_1_10_MBARE           —— 未开分页：恒等映射
    237  for (i = 0; i < levels; ...  —— 三级查表主循环
    252  pte = ldq_phys(...)          —— 读页表项（8 字节）
    256  if (!(pte & PTE_V))          —— 无效项
    259  else if (!(pte & (R|W|X)))   —— 内部项，base = ppn << 12 继续下一级
    333  vpn = addr >> 12             —— 大页：把剩余 VPN 位当页内偏移
    334  *physical = (ppn | ...) << 12
    349  return TRANSLATE_SUCCESS
MANUAL
}

make_native_script() {
    cat >"$NATIVE_GDB_SCRIPT" <<EOF
set pagination off
set confirm off
set breakpoint pending on
handle SIGPIPE nostop noprint pass
file $QEMU_BIN
dir $QEMU_SRC

# ① 裸机阶段：内核入口还在物理地址 0x80200000 上取指（satp=0）
break get_physical_address if addr == 0x80200000
run $QEMU_ARGS -serial file:$SERIAL_LOG

printf "\n\n############ [1] 分页未开启时的取指 ############\n"
printf "guest 地址 addr = %#lx（内核入口的物理地址）\n", addr
printf "satp = %#lx  priv = %d (1=S)  access_type = %d (2=取指)\n", env->satp, env->priv, access_type
bt 4
disable 1
tbreak $QEMU_SRC/target/riscv/cpu_helper.c:197
continue
printf "  → 停在 197 行 case VM_1_10_MBARE：*physical = addr，等同不翻译\n"

# ② 开分页后：jr 到虚拟地址 kern_init，第一次真正的地址翻译
break get_physical_address if addr == 0xffffffffc02000d6
continue
printf "\n\n############ [2] 开分页后的取指（PC = kern_init 虚拟地址）############\n"
printf "guest 地址 addr = %#lx\n", addr
printf "satp = %#lx  (MODE=%d, PPN=%#lx)\n", env->satp, (long)(env->satp >> 60), (long)(env->satp & 0xFFFFFFFFFFFFF)
printf "基址 base = satp.PPN << 12 = %#lx（根表物理地址）\n", (env->satp & 0xFFFFFFFFFFFFF) << 12
bt 5
printf "\n---- 开始走页表（在 256 行打印每一级的索引/地址/页表项）----\n"
break $QEMU_SRC/target/riscv/cpu_helper.c:256 if addr == 0xffffffffc02000d6
commands
silent
printf "  第 %d 级: idx=%#lx  pte_addr=%#lx  pte=%#lx  ppn=%#lx\n", i + 1, idx, pte_addr, pte, ppn
continue
end
break $QEMU_SRC/target/riscv/cpu_helper.c:334 if addr == 0xffffffffc02000d6
continue
printf "  → 命中叶子项（大页）：vpn=%#lx  ptshift=%d  ppn=%#lx\n", vpn, ptshift, ppn
printf "  → 计算：*physical = (ppn | (vpn & ((1<<ptshift)-1))) << 12 = %#lx\n", (ppn | (vpn & ((1L << ptshift) - 1))) << 12
printf "  → 加上页内偏移 %#lx，最终访问的物理地址 = %#lx\n", (addr & 0xFFF), ((ppn | (vpn & ((1L << ptshift) - 1))) << 12) + (addr & 0xFFF)
printf "\n说明：本实验的根表第 511 项就是 1 GiB 大页叶子项，所以循环只走了 1 级；\n"
printf "      如果换成 4 KiB 粒度页表，就会看到 3 级、每级 idx/pte_addr/pte 各一组。\n"
kill
quit
printf "\n说明：本次只走了 1 级 —— 根表第 511 项就是 1 GiB 大页叶子项\n"
kill
quit
EOF
}

run_native_bg() {
    info "启动 x86_64 gdb 驱动 QEMU（QEMU 以 -s -S 起，等 riscv gdb 连上）"
    nohup gdb -q -batch -x "$NATIVE_GDB_SCRIPT" >"$NATIVE_LOG" 2>&1 &
    NATIVE_PID=$!
    for _ in $(seq 1 40); do
        if grep -q "Remote debugging\|Breakpoint 1 at" "$NATIVE_LOG" 2>/dev/null; then break; fi
        sleep 0.3
    done
    sleep 1
}

run_rv_gdb() {
    info "启动 riscv gdb 连接 gdbstub 并让内核运行"
    cat >"$RV_GDB_SCRIPT" <<'EOF'
set confirm off
set pagination off
file bin/kernel
set arch riscv:rv64
target remote localhost:1234
continue
quit
EOF
    timeout 40 "$RV_GDB" -batch -x "$RV_GDB_SCRIPT" >"$RV_LOG" 2>&1
}

cmd_run() {
    [ -x "$QEMU_BIN" ] || { warn "找不到 $QEMU_BIN"; exit 1; }
    command -v gdb >/dev/null || { warn "缺 x86_64 gdb，请先 sudo apt install gdb"; exit 1; }
    command -v "$RV_GDB" >/dev/null || { warn "找不到 $RV_GDB"; exit 1; }
    [ -d "$QEMU_SRC" ] || { warn "找不到 QEMU 源码目录 $QEMU_SRC（gdb 需要它显示源码）"; exit 1; }
    if ! file "$QEMU_BIN" | grep -q "with debug_info"; then
        warn "$QEMU_BIN 没有调试信息，需要 --enable-debug 重新编译"
        exit 1
    fi
    if [ ! -f bin/ucore.img ]; then
        info "编译内核 ..."
        make >/dev/null 2>&1 || { warn "编译失败"; exit 1; }
    fi

    pkill -x qemu-system-riscv64 2>/dev/null
    sleep 1
    if (exec 3<>/dev/tcp/127.0.0.1/1234) 2>/dev/null; then
        exec 3<&- 2>/dev/null
        warn "端口 1234 仍被占用，请先结束残留的 qemu/gdb"
        exit 1
    fi
    make_native_script
    run_native_bg
    run_rv_gdb
    sleep 1
    wait "$NATIVE_PID" 2>/dev/null

    echo
    ok "=== 观察记录（来自 QEMU 源码层面的 x86_64 gdb）==="
    sed -n '/############ \[1\]/,$p' "$NATIVE_LOG"
    if [ -s "$SERIAL_LOG" ]; then
        echo
        info "（同一时刻内核串口输出的最后一行：$(grep -a . "$SERIAL_LOG" | tail -1)）"
    fi
}

case "${1:-run}" in
    run)    cmd_run ;;
    manual) cmd_manual ;;
    *)      warn "未知参数：$1"; echo "用法：./demo-mmu.sh [run|manual]"; exit 1 ;;
esac
