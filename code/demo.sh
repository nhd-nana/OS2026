#!/usr/bin/env bash
#
# Lab1 演示脚本 —— 最小可执行内核（riscv64-ucore）
#
# 用法（在本目录下执行）：
#   ./demo.sh run            清理后完整编译（显示 cc/ld/objcopy 全过程）并运行内核
#                            （前台；退出：Ctrl+A 松开后再按 X）
#   ./demo.sh run --log      同上，并把输出同时存到 /tmp/lab1_demo_run.log
#   ./demo.sh gdb            自动起 QEMU(-s -S) 并进入 GDB，自动演示启动流程，
#                            演示完停在 GDB 提示符，可继续自己敲命令
#   ./demo.sh gdb --batch    同 gdb，但执行完自动退出（用于留日志 / 自测）
#   ./demo.sh kill           清理残留的 QEMU 进程
#   ./demo.sh help           显示本帮助
#
set -u

cd "$(dirname "$0")"

PORT="${PORT:-1234}"
GDB="${GDB:-riscv64-unknown-elf-gdb}"
QEMU="${QEMU:-qemu-system-riscv64}"
IMG=bin/ucore.img
PIDFILE=/tmp/lab1_demo_qemu.pid
LOGFILE=/tmp/lab1_demo_qemu.log
GDBSCRIPT=/tmp/lab1_demo.gdb
RUNLOG=/tmp/lab1_demo_run.log

info() { printf '\033[36m[demo]\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m[demo]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[demo]\033[0m %s\n' "$*"; }

# 静默编译：只报结果，用于 gdb 演示（不需要刷一屏编译输出）
build() {
    info "编译中 ..."
    if ! make >/dev/null; then
        warn "编译失败，请先修好再演示"
        exit 1
    fi
    ok "编译完成：$IMG（$(stat -c%s "$IMG") 字节 = end - 0x80200000）"
}

# 完整编译：先 clean 再编译，让交叉编译链路（cc → ld → objcopy）完整显示，
# 用于 run 演示 —— 这是“代码是怎么变成镜像的”的直接证据。
build_verbose() {
    info "清理后重新编译 ..."
    make clean >/dev/null 2>&1
    if ! make; then
        warn "编译失败，请先修好再演示"
        exit 1
    fi
    ok "编译完成：$IMG（$(stat -c%s "$IMG") 字节 = end - 0x80200000）"
}

kill_qemu() {
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
    fi
    pkill -f 'qemu-system-riscv64.*ucore.img' 2>/dev/null
    sleep 0.3
}

wait_port() {
    for _ in $(seq 1 60); do
        if (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; then
            exec 3<&- 2>/dev/null
            return 0
        fi
        sleep 0.2
    done
    return 1
}

check_tools() {
    command -v "$QEMU" >/dev/null || { warn "找不到 $QEMU（需在 WSL/Linux 侧执行）"; exit 1; }
    command -v "$GDB"  >/dev/null || { warn "找不到 $GDB"; exit 1; }
}

cmd_run() {
    check_tools
    build_verbose
    cat <<'TIP'

======================================================================
 演示要点（出现下面两行即可证明启动成功）
   ① Domain0 Next Address : 0x0000000080200000
      → OpenSBI 要把控制权交给 0x80200000 的内核
   ② (THU.CST) os is loading ...
      → 内核第一条指令已执行，并通过 SBI 的 console 服务打印
   之后内核停在 while(1)。退出 QEMU：Ctrl+A 松开后再按 X
======================================================================

TIP
    case "${1:-}" in
        --log)
            info "输出同时保存到 $RUNLOG"
            "$QEMU" -machine virt -nographic -bios default -kernel "$IMG" 2>&1 | tee "$RUNLOG"
            ;;
        *)
            exec "$QEMU" -machine virt -nographic -bios default -kernel "$IMG"
            ;;
    esac
}

cmd_gdb() {
    local batch="$1"
    check_tools
    build
    kill_qemu

    info "后台启动 QEMU：-s（开放 gdbstub:$PORT） -S（复位即停，等 GDB 接管）"
    # stdin 接 /dev/null：避免 QEMU 抢占终端的键盘输入（GDB 要用）
    nohup "$QEMU" -machine virt -nographic -bios default -kernel "$IMG" -s -S \
        < /dev/null >"$LOGFILE" 2>&1 &
    echo $! > "$PIDFILE"
    trap kill_qemu EXIT

    if ! wait_port; then
        warn "等不到 gdbstub:$PORT；QEMU 日志见 $LOGFILE"
        exit 1
    fi
    ok "QEMU 已就绪（PID $(cat "$PIDFILE")）"

    cat >"$GDBSCRIPT" <<'GDBEOF'
set confirm off
set pagination off
file bin/kernel
set arch riscv:rv64
target remote localhost:1234

echo \n\n>>> [1] 复位后的 PC：应为 0x1000（复位向量 / MROM）\n
info registers pc

echo \n>>> [2] MROM 的全部指令：只有 6 条，最后一条 jr 跳向 OpenSBI\n
x/6i $pc

echo \n>>> [3] 单步执行这 6 条，控制权进入 OpenSBI（0x80000000）\n
si 6
info registers pc

echo \n>>> [4] 在“内核第一条指令”处下断点，然后继续\n
b *0x80200000
c

echo \n>>> [5] 交接瞬间：pc 是内核入口，sp 仍是 OpenSBI 自己的栈(0x8003xxxx)\n
info registers pc sp

echo \n>>> [6] 单步 3 条：la sp,bootstacktop 建栈 + tail kern_init 移交控制权\n
si 3

echo \n>>> [7] 已进入 C 入口：pc=kern_init，sp=bootstacktop(0x80203000)\n
info registers pc sp

echo \n>>> 演示结束。下面可自由调试；想看内核运行结果就先 quit，再执行 ./demo.sh run\n
GDBEOF

    if [ "$batch" = "--batch" ] || [ "$batch" = "1" ]; then
        echo quit >>"$GDBSCRIPT"
        info "GDB 批处理模式运行 ..."
    else
        info "GDB 交互模式：上面 7 步会自动跑一遍，之后可自己敲命令（退出用 quit）"
    fi
    echo
    "$GDB" -q -x "$GDBSCRIPT"
}

cmd_help() {
    sed -n '3,12p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-help}" in
    run)   cmd_run "${2:-}" ;;
    gdb)   cmd_gdb "${2:-}" ;;
    kill)  kill_qemu; ok "已清理残留 QEMU 进程" ;;
    help|-h|--help) cmd_help ;;
    *)     warn "未知参数：$1"; echo; cmd_help; exit 1 ;;
esac
