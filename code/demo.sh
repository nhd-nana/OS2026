#!/usr/bin/env bash
#
# Lab2 演示脚本（物理内存与页表）—— 只在 WSL 里运行
#
# 用法（在本目录下执行）：
#   ./demo.sh run         清理后完整编译并运行内核（QEMU 前台，Ctrl+A 松手再按 X 退出）
#   ./demo.sh grade       运行自动评分（注入测试 + grade.sh），显示两个 PASS 与总分
#   ./demo.sh buddy       以 PMM=buddy 增量重编（只重编 pmm.c），跑 8 个黑盒用例，结束后自动恢复默认
#   ./demo.sh slub        运行默认构建，提取 SLUB 自测输出
#   ./demo.sh mmu         分支任务：双 GDB 自动演示页表查询（约 10 秒）
#   ./demo.sh mmu-manual  只打印分支任务的手工三终端步骤
#   ./demo.sh all         依次执行 grade + buddy + slub
#   ./demo.sh help        显示本帮助
#
# 说明：
#   - 仓库源码不会被修改；buddy 演示通过 make PMM=buddy 加编译宏切换分配器，
#     Makefile 用 stamp 文件感知 PMM 变化，只重编 pmm.c，跑完自动重编回默认。
#   - QEMU 版本由 Makefile 自动选择：优先 $HOME/opt/qemu-4.1.1，缺失才退回 PATH。
#   - mmu 需要：带调试信息的 QEMU 4.1.1、QEMU 源码目录、x86_64 gdb、riscv gdb。
# ===HELP-END===
set -u

cd "$(dirname "$0")" || exit 1

QEMU_BIN="${QEMU_BIN:-$HOME/opt/qemu-4.1.1/bin/qemu-system-riscv64}"
[ -x "$QEMU_BIN" ] || QEMU_BIN="$(command -v qemu-system-riscv64 || true)"
RUN_TIMEOUT=20
MMU_SRC="${QEMU_SRC:-$HOME/qemu-4.1.1-build/qemu-4.1.1}"
RV_GDB="${RV_GDB:-riscv64-unknown-elf-gdb}"

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
    # $1=日志路径；$2=出现即提前结束的正则（可选，省去等满超时）
    # 内核是死循环，只能靠信号结束；输出落盘而不是接管道，避免管道被挂住
    local log="$1" pattern="${2:-}"
    timeout --signal=KILL "$RUN_TIMEOUT" "$QEMU_BIN" \
        -machine virt -nographic -bios default -kernel bin/ucore.img \
        </dev/null >"$log" 2>&1 &
    local pid=$!
    if [ -n "$pattern" ]; then
        local i=0
        while [ "$i" -lt 200 ] && kill -0 "$pid" 2>/dev/null; do
            grep -aq "$pattern" "$log" 2>/dev/null && break
            sleep 0.1
            i=$((i + 1))
        done
        sleep 0.3          # 留点时间让尾部输出落盘
        kill "$pid" 2>/dev/null
    fi
    wait "$pid" 2>/dev/null
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
 以下三行不是报错，是预期输出：
   buddy_test: ... not buddy; skip ...      ← 默认非 Buddy，测试自我保护跳过
   slub_free: invalid or duplicate ...      ← SLUB 非法释放用例的正常输出
   slub_test: ... checks passed             ← SLUB 自测通过
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
    run_kernel /tmp/lab2_demo_slub.log "slub_test: .*checks passed"
    echo
    grep -a -E "memory management|check_alloc_page|buddy_test:|slub" /tmp/lab2_demo_slub.log || warn "未捕获到预期输出"
    echo
    info "说明：'buddy_test: ... not buddy; skip' 是默认非 Buddy 时的自我保护；'slub_free: ...' 是非法释放用例的预期输出"
    if grep -aq "slub_test: allocation, reuse, bounds, exhaustion and invalid-free checks passed" /tmp/lab2_demo_slub.log; then
        ok "SLUB 自测通过"
    else
        warn "SLUB 自测未通过"
    fi
}

cmd_buddy() {
    expect_tools
    info "以 PMM=buddy 增量重编（只重编 pmm.c，源码不动）..."
    make PMM=buddy >/dev/null 2>&1 || { warn "Buddy 构建失败"; return 1; }
    run_kernel /tmp/lab2_demo_buddy.log "buddy_test: .*passed"

    echo
    grep -a -E "memory management|buddy_pmm_check|check_alloc_page|buddy_test" /tmp/lab2_demo_buddy.log
    echo
    if grep -aq "8/8 passed, 0 failed" /tmp/lab2_demo_buddy.log; then
        ok "Buddy 8 个用例全部通过"
        info "说明：'no matching allocation record' 等提示来自非法释放用例，属预期"
    else
        warn "Buddy 用例未全部通过"
    fi

    info "恢复默认构建（Best-Fit）..."
    make >/dev/null 2>&1 || warn "恢复默认构建失败，请手动执行 make"
}

# ---------- 分支任务：双 GDB 调试页表查询 ----------

mmu_manual() {
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
    (gdb) dir <QEMU 源码目录>               # 让 gdb 找到源码
    (gdb) break get_physical_address if addr == 0xffffffffc02000d6
    (gdb) break <QEMU 源码目录>/target/riscv/cpu_helper.c:256
    (gdb) continue                          # 之后等终端 3 让内核跑起来

终端 3 —— riscv 交叉 gdb 连上 gdbstub，让内核执行：
    cd <仓库根>/code
    make gdb                        # 或 riscv64-unknown-elf-gdb -ex 'target remote localhost:1234'
    (gdb) continue                  # 内核一开始跑，终端 2 就会命中断点

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

mmu_cleanup() {
    pkill -x qemu-system-riscv64 2>/dev/null
    if [ "${KEEP_LOG:-0}" = "1" ]; then
        info "日志保留在 $MMU_LOG_DIR"
    else
        rm -rf "$MMU_LOG_DIR"
    fi
}

cmd_mmu() {
    [ -x "$QEMU_BIN" ] || { warn "找不到 $QEMU_BIN"; return 1; }
    command -v gdb >/dev/null || { warn "缺 x86_64 gdb，请先 sudo apt install gdb"; return 1; }
    command -v "$RV_GDB" >/dev/null || { warn "找不到 $RV_GDB"; return 1; }
    [ -d "$MMU_SRC" ] || { warn "找不到 QEMU 源码目录 $MMU_SRC（gdb 需要它显示源码）"; return 1; }
    file "$QEMU_BIN" | grep -q "with debug_info" ||
        { warn "$QEMU_BIN 没有调试信息，需 --enable-debug 重新编译"; return 1; }
    [ -f bin/ucore.img ] || { info "编译内核 ..."; make >/dev/null 2>&1 || { warn "编译失败"; return 1; }; }

    MMU_LOG_DIR="$(mktemp -d /tmp/lab2_mmu.XXXXXX)"
    local native_gdb="$MMU_LOG_DIR/native.gdb" rv_gdb="$MMU_LOG_DIR/rv.gdb"
    local native_log="$MMU_LOG_DIR/native.log" rv_log="$MMU_LOG_DIR/rv.log"
    local serial_log="$MMU_LOG_DIR/serial.log"
    local qemu_args="-machine virt -nographic -bios default -kernel bin/ucore.img -s -S -monitor none"

    cat >"$native_gdb" <<EOF
set pagination off
set confirm off
set breakpoint pending on
handle SIGPIPE nostop noprint pass
file $QEMU_BIN
dir $MMU_SRC

# ① 裸机阶段：内核入口还在物理地址 0x80200000 上取指（satp=0）
break get_physical_address if addr == 0x80200000
run $qemu_args -serial file:$serial_log

printf "\n\n############ [1] 分页未开启时的取指 ############\n"
printf "guest 地址 addr = %#lx（内核入口的物理地址）\n", addr
printf "satp = %#lx  priv = %d (1=S)  access_type = %d (2=取指)\n", env->satp, env->priv, access_type
bt 4
disable 1
tbreak $MMU_SRC/target/riscv/cpu_helper.c:197
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
break $MMU_SRC/target/riscv/cpu_helper.c:256 if addr == 0xffffffffc02000d6
commands
silent
printf "  第 %d 级: idx=%#lx  pte_addr=%#lx  pte=%#lx  ppn=%#lx\n", i + 1, idx, pte_addr, pte, ppn
continue
end
break $MMU_SRC/target/riscv/cpu_helper.c:334 if addr == 0xffffffffc02000d6
continue
printf "  → 命中叶子项（大页）：vpn=%#lx  ptshift=%d  ppn=%#lx\n", vpn, ptshift, ppn
printf "  → 计算：*physical = (ppn | (vpn & ((1<<ptshift)-1))) << 12 = %#lx\n", (ppn | (vpn & ((1L << ptshift) - 1))) << 12
printf "  → 加上页内偏移 %#lx，最终访问的物理地址 = %#lx\n", (addr & 0xFFF), ((ppn | (vpn & ((1L << ptshift) - 1))) << 12) + (addr & 0xFFF)
printf "\n说明：本实验的根表第 511 项就是 1 GiB 大页叶子项，所以循环只走了 1 级；\n"
printf "      若换成 4 KiB 粒度页表，会看到 3 级、每级 idx/pte_addr/pte 各一组。\n"
kill
quit
EOF

    pkill -x qemu-system-riscv64 2>/dev/null
    sleep 1
    if (exec 3<>/dev/tcp/127.0.0.1/1234) 2>/dev/null; then
        exec 3<&- 2>/dev/null
        warn "端口 1234 仍被占用，请先结束残留的 qemu/gdb"
        rm -rf "$MMU_LOG_DIR"
        return 1
    fi

    info "启动 x86_64 gdb 驱动 QEMU（QEMU 以 -s -S 起，等 riscv gdb 连上）"
    nohup gdb -q -batch -x "$native_gdb" >"$native_log" 2>&1 &
    local native_pid=$!
    for _ in $(seq 1 40); do
        grep -q "Remote debugging\|Breakpoint 1 at" "$native_log" 2>/dev/null && break
        sleep 0.3
    done
    sleep 1

    info "启动 riscv gdb 连接 gdbstub 并让内核运行"
    cat >"$rv_gdb" <<'EOF'
set confirm off
set pagination off
file bin/kernel
set arch riscv:rv64
target remote localhost:1234
continue
quit
EOF
    timeout 40 "$RV_GDB" -batch -x "$rv_gdb" >"$rv_log" 2>&1
    sleep 1
    wait "$native_pid" 2>/dev/null

    echo
    ok "=== 观察记录（来自 QEMU 源码层面的 x86_64 gdb）==="
    sed -n '/############ \[1\]/,$p' "$native_log"
    if [ -s "$serial_log" ]; then
        echo
        info "（同一时刻内核串口输出最后一行：$(grep -a . "$serial_log" | tail -1)）"
    fi
    mmu_cleanup
}

cmd_help() {
    sed -n '/^# 用法/,/^# ===HELP-END===/{/^# ===HELP-END===/d;p}' "$0" | sed 's/^# \{0,1\}//'
}

case "${1:-help}" in
    run)   cmd_run ;;
    grade) cmd_grade ;;
    slub)  cmd_slub ;;
    buddy) cmd_buddy ;;
    mmu)        cmd_mmu ;;
    mmu-manual) mmu_manual ;;
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
