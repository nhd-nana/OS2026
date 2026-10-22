#!/usr/bin/env bash
#
# Lab2 演示脚本（物理内存与页表）—— 只在 WSL 里运行
#
# 用法（在本目录下执行）：
#   ./demo.sh run       清理后完整编译并运行内核（QEMU 前台，Ctrl+A 松手再按 X 退出）
#   ./demo.sh grade     运行自动评分（注入测试 + grade.sh），显示两个 PASS 与总分
#   ./demo.sh buddy     在临时副本里把分配器切到 Buddy，跑 8 个黑盒用例（不改动仓库）
#   ./demo.sh slub      运行默认构建，提取 SLUB 自测输出
#   ./demo.sh all       依次执行 grade + buddy + slub（约 1 分钟，用于快速自检）
#   ./demo.sh help      显示本帮助
#
# 说明：
#   - 仓库代码不会被修改；buddy 演示在 mktemp 目录里切 pmm_manager，跑完即删。
#   - QEMU 版本由 Makefile 自动选择：优先 $HOME/opt/qemu-4.1.1，缺失才退回 PATH。
set -u

cd "$(dirname "$0")" || exit 1

QEMU_BIN="${QEMU_BIN:-$HOME/opt/qemu-4.1.1/bin/qemu-system-riscv64}"
[ -x "$QEMU_BIN" ] || QEMU_BIN="$(command -v qemu-system-riscv64 || true)"
RUN_TIMEOUT=20

info() { printf '\033[36m[demo]\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m[demo]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[demo]\033[0m %s\n' "$*"; }

expect_tools() {
    [ -n "$QEMU_BIN" ] || { warn "找不到 qemu-system-riscv64（需在 WSL 内运行）"; exit 1; }
    command -v make >/dev/null || { warn "找不到 make"; exit 1; }
}

build_verbose() {
    info "清理后重新编译（显示 cc / ld / objcopy 全过程）"
    make clean >/dev/null 2>&1
    make || { warn "编译失败"; exit 1; }
    ok "编译完成：$(stat -c%s bin/ucore.img 2>/dev/null) 字节"
}

run_kernel() {
    # 内核是死循环，用 timeout 发 SIGTERM 结束（QEMU 会正常退出，不会打印 Killed）；
    # 输出落盘而不是接管道，避免管道被挂住
    timeout "$RUN_TIMEOUT" "$QEMU_BIN" \
        -machine virt -nographic -bios default -kernel bin/ucore.img \
        </dev/null >"$1" 2>&1
    return 0
}

cmd_run() {
    expect_tools
    build_verbose
    cat <<'TIP'

======================================================================
 现场演示请看这几行（顺序即启动链）
   DTB Address / Physical Memory from DTB   ← 内存范围来自设备树
   (THU.CST) os is loading ...              ← 内核 C 入口已执行
   memory management: best_fit_pmm_manager  ← 分配器已切换
   phys memory / npage / free pages         ← page_init 的三项关键结果
   check_alloc_page() succeeded!            ← 分配/释放自检通过
   satp virtual/physical address            ← 虚实映射生效
 退出：Ctrl+A 松手，再按 X
======================================================================

TIP
    "$QEMU_BIN" -machine virt -nographic -bios default -kernel bin/ucore.img
}

cmd_grade() {
    expect_tools
    info "运行自动评分（make grade）..."
    out="$(make grade 2>&1)"
    echo "$out" | tail -5
    if echo "$out" | grep -q "Total Score: 20/20"; then
        ok "评分通过：20/20"
    else
        warn "评分未满分，请检查上面输出"
    fi
}

cmd_slub() {
    expect_tools
    # 注意：make grade 会在当前目录 make clean，所以这里必须先确认镜像存在
    if [ ! -f bin/ucore.img ]; then
        info "bin/ucore.img 不存在（grade 会清理），先编译..."
        make >/dev/null 2>&1 || { warn "编译失败"; exit 1; }
    fi
    info "运行默认构建（SLUB 自测在 kern_init() 里自动执行）..."
    run_kernel /tmp/lab2_demo_slub.log
    echo
    grep -a -E "memory management|check_alloc_page|buddy_test:|slub" /tmp/lab2_demo_slub.log || warn "未捕获到预期输出"
    echo
    if grep -aq "slub_test: allocation, reuse, bounds, exhaustion and invalid-free checks passed" /tmp/lab2_demo_slub.log; then
        ok "SLUB 自测通过（前两行 slub_free 提示是非法释放用例的预期输出）"
    else
        warn "SLUB 自测未通过"
    fi
}

cmd_buddy() {
    expect_tools
    local tmp
    tmp="$(mktemp -d /tmp/lab2_buddy.XXXXXX)"
    trap 'rm -rf "$tmp"' RETURN
    info "复制到临时目录：$tmp（仓库不会被改动）"
    cp -r . "$tmp/src" || { warn "复制失败"; return 1; }
    cd "$tmp/src" || return 1
    rm -rf obj bin

    sed -i 's/pmm_manager = &best_fit_pmm_manager;/pmm_manager = \&buddy_pmm_manager;/' kern/mm/pmm.c
    if ! grep -q "buddy_pmm_manager;" kern/mm/pmm.c; then
        warn "切换失败：pmm.c 里没找到 best_fit_pmm_manager 赋值"
        cd - >/dev/null; return 1
    fi

    make >/dev/null 2>&1 || { warn "Buddy 构建失败"; cd - >/dev/null; return 1; }
    run_kernel "$tmp/buddy.log"

    echo
    grep -a -E "memory management|buddy_pmm_check|check_alloc_page|buddy_test" "$tmp/buddy.log"
    echo
    if grep -aq "8/8 passed, 0 failed" "$tmp/buddy.log"; then
        ok "Buddy 8 个用例全部通过"
        info "说明：'no matching allocation record' 等提示来自非法释放用例，属预期"
    else
        warn "Buddy 用例未全部通过"
    fi
    cd - >/dev/null
}

cmd_help() {
    sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-help}" in
    run)   cmd_run ;;
    grade) cmd_grade ;;
    slub)  cmd_slub ;;
    buddy) cmd_buddy ;;
    all)
        cmd_grade
        echo
        cmd_buddy
        echo
        cmd_slub
        ;;
    help|-h|--help) cmd_help ;;
    *) warn "未知参数：$1"; echo; cmd_help; exit 1 ;;
esac
