# Lab3 提示词记录

每个模块按四段式记录实际用过的提示词：

```
[PROMPT]        任务与操作要求
[RELY]          可依赖的宏 / 结构 / 函数
[GUARANTEE]     必须实现的接口
[SPECIFICATION] Pre/Post-Condition 与 Case
```

---

## 功能模块一：Trap 入口与上下文恢复（`trapentry.S`）

**负责**：2412133-吕鹏哲

### 生成实现的提示词

[PROMPT]
在 `kern/trap/trapentry.S` 中，只补两处 `#lab3 function1 YOUR CODE` 占位，其余（`SAVE_ALL`、文件头部 `#define sbadaddr stval` 等）一律不动：

1. `.macro RESTORE_ALL` 与 `.endm` 之间——按 `SAVE_ALL` 的保存关系逆序恢复 36 个槽位；
2. `__alltraps` 标签内、`SAVE_ALL` 之后——把 trapframe 指针按 RISC-V 调用约定传给 `trap()` 并调用。

背景：`SAVE_ALL` 已在栈顶建好 36 槽 trapframe（每槽 `REGBYTES`=8 字节）：偏移 0~31 依次是 x0..x31（顺序同 `struct pushregs`），偏移 32/33/34/35 依次是 `sstatus`/`sepc`/`badvaddr`/`cause`。

要求：

- `RESTORE_ALL` 先处理两个 CSR 槽位：偏移 32（sstatus）、33（sepc）不能 `LOAD` 直写 CSR，必须先 `LOAD` 到某个通用寄存器再 `csrw`；`csrw sstatus`、`csrw sepc` 都要在 `sret` 之前完成。
- 用 sstatus 的 `SSTATUS_SPP` 位判断返回特权级：SPP==1（内核态）保持 `sscratch` 不动（此时为 0）；SPP==0（用户态，留待 lab4）先 `csrw sscratch, sp` 存下内核 trapframe 指针。用 `andi` 取位 + `bnez` 跳过。
- 通用寄存器 `LOAD x<i>, <i>*REGBYTES(sp)` 逐个恢复（i = 1,3,4,...,31），跳过 x0（硬连线 0）；**x2(sp) 必须最后恢复**（它同时是所有 `LOAD` 的基址），从 `2*REGBYTES(sp)` 读。
- `__alltraps` 里 `SAVE_ALL` 之后 `sp` 已指向 trapframe：`mv a0, sp`（a0 是第一参数），再 `jal trap`；`trap` 是 C 函数，`sp` 属 callee-saved，返回后 `sp` 不变，直接落到紧随其后的 `__trapret`（`RESTORE_ALL` + `sret`）。不要手改 `sp`、不要重复调用 `RESTORE_ALL`。

禁止：不改 `SAVE_ALL`、`struct trapframe`/`struct pushregs`、`trap()` 签名、`__trapret` 的 `sret`，不新增文件。

[RELY]

| 依赖 | 位置 | 提供什么 |
|---|---|---|
| `REGBYTES`(=8) | `libs/riscv.h` | 槽位字节大小，偏移写作 `i*REGBYTES(sp)` |
| `LOAD`(=ld) / `STORE`(=sd) | `libs/riscv.h` | RV64 载入/存储宏 |
| `SAVE_ALL` | `kern/trap/trapentry.S` | 已提供的保存序列，恢复布局与逆序的唯一依据 |
| `SSTATUS_SPP` = 0x00000100 | `libs/riscv.h` | sstatus 中判定返回特权级的位 |
| `struct trapframe` / `struct pushregs` | `kern/trap/trap.h` | 36 槽布局；gpr 顺序 = x0..x31 |
| `void trap(struct trapframe *tf)` | `kern/trap/trap.c` | C 层入口，第一参数经 `a0` 传递 |
| `__trapret` | `kern/trap/trapentry.S` | `RESTORE_ALL` + `sret` 的返回路径 |
| `sret` | RISC-V 特权指令 | 依据 `sepc`/`sstatus` 返回被中断执行流 |

[GUARANTEE]

```asm
.macro RESTORE_ALL      # 输入：sp 指向 trapframe；输出：寄存器/CSR 恢复，sp 回到原栈顶
.endm

__alltraps:             # Trap 入口：SAVE_ALL -> 传 tf 给 trap() -> 落到 __trapret -> sret
```

对外接口链保持：`__alltraps` → `trap(struct trapframe *tf)` → `__trapret` → `sret`，任何一环的签名与调用约定不得改变。

[SPECIFICATION]

- **Pre-condition**：`SAVE_ALL` 已执行，`sp` 指向 36 槽 trapframe；`sscratch` 被清为 0；槽位 2（`tf->gpr.sp`）保存压栈前原始 `sp`；槽位 32/33 保存 `sstatus`/`sepc`。
- **Post-condition**：x1,x3..x31、`sp`、`sstatus`、`sepc` 全部恢复到 Trap 前值，`sret` 正确跳回 `sepc`；内核态返回后 `sscratch` 仍为 0。
- **Case A（内核态返回，SPP=1，本实验主要场景）**：`ebreak`/非法指令/定时器中断均来自 S-mode，走 `andi + bnez` 跳过 sscratch 分支，26 个寄存器逐个原样恢复（Test 1 逐位比对）。
- **Case B（用户态返回，SPP=0，预留）**：执行 `csrw sscratch, sp`，本实验不触发，仅要求路径正确且不影响 Case A。
- **Case C（异常推进 sepc）**：`exception_handler` 已 `tf->epc += 4`，`RESTORE_ALL` 必须重读槽位 33 并 `csrw sepc`，使 `sret` 跳过触发异常的指令，避免同一条指令反复异常。
- **不变式**：每个 `LOAD` 偏移与 `SAVE_ALL` 对应 `STORE` 偏移严格相等；`sp` 走槽位 2 且放在所有 `LOAD` 之后恢复。

### 生成测试的提示词

[PROMPT]
不要另写测试文件，用课程自带的 `make grade` 验收，只看 Test 1（`GRADE_LAB3_CONTEXT`，35 分）。先读 `tools/test_lab3_context.S` 与 `tools/grade.sh` 弄清判定逻辑：

`test_lab3_register_context()` 给 26 个寄存器赋 0x105..0x11f 后执行 `ebreak`，返回后逐个 `bne` 与期望值比对，全部相等返回 0；`test_lab3_exception_context()` 据此打印 `[Test 1] Trap entry and breakpoint return: PASS (35/35)`，否则打印 `FAIL (register context)`。`grade.sh` 用 `grep -F` 精确匹配 PASS 字符串与 `Breakpoint at` 两个字符串。

[GUARANTEE]
测试只依赖串口输出字符串与测试函数自己的返回值，不读内核内部变量；不要为了通过而改动 `tools/` 下的测试文件。

[SPECIFICATION]

- **通过条件（Test 1，35 分）**：串口同时命中 `[Test 1] Trap entry and breakpoint return: PASS (35/35)` 与 `Breakpoint at`。
- **失败形态 1（能进不能出）**：只打印 `Breakpoint at`、没有 PASS/FAIL → `RESTORE_ALL` 为空或 `sret` 没返回，QEMU 超时。
- **失败形态 2（能出但寄存器错）**：打印 `FAIL (register context)` → Trap 能返回，但 26 个寄存器中有错位（本例的真实原因见 `report.md` 模块一：`ebreak` 被压缩成 2 字节 `c.ebreak`，`epc += 4` 跳过头）。
- **失败形态 3（没进 Trap）**：连 `Breakpoint at` 都没有 → `__alltraps` 没接上 `trap()` 或 `stvec` 没指向入口。

### 测试覆盖说明

| 覆盖点 | 测试手段 | 期望结果 |
|---|---|---|
| Trap 入口建立 | `ebreak` 触发 `__alltraps` | 打印 `Breakpoint at` |
| trapframe 传参 | `mv a0, sp` + `jal trap` | C 层拿到正确 tf，`tf->epc += 4` 生效 |
| 26 个通用寄存器恢复 | 赋 0x105..0x11f 后 `ebreak` 逐位比对 | 全部相等，返回 0 |
| sp 特殊恢复 | x2 走槽位 2 且最后恢复 | 栈指针回原位置 |
| CSR 恢复 | sstatus/sepc 经 GPR 中转 `csrw` | `sret` 回到 `ebreak` 下一条指令 |

---

## 功能模块二：中断与异常处理（`trap.c`）

**负责**：2412679-钟一成（`interrupt_handler`）、2412799-葛熠（`exception_handler`）

### 生成实现的提示词

[PROMPT]
在当前 uCore Lab3 的 `kern/trap/trap.c` 中，把 `//lab3 function2` 标记下层只调用 `print_trapframe(tf)` 的占位实现，替换为完整的 `interrupt_handler(struct trapframe *tf)`。要求：

1. 先掩掉 `tf->cause` 的最高位（bit 63，中断标志位）再分发；
2. 时钟中断（`IRQ_S_TIMER`，5）：**每次中断都调用 `clock_set_next_event()` 重装定时器**，然后 `ticks++`；每 `TICK_NUM`（100）的整数倍调用 `print_ticks()`；累计打印达到 `SHUTDOWN_AFTER` 次后调用 `sbi_shutdown()`；
3. 软件中断（`IRQ_S_SOFT`，1）：打印 `Supervisor software interrupt`；
4. 外部中断（`IRQ_S_EXT`，9）：打印分类提示；
5. 其余：打印 `Unknown interrupt: cause <十进制数>`，并调用 `print_trapframe(tf)`；
6. 除了本函数与该文件头部的 `#include`，不要修改任何东西：不动 `trap_dispatch()` 的分发逻辑、不动 `print_ticks()` 的格式、不动 `exception_handler()`、不动 `trap()` 的签名。

[RELY]

| 依赖 | 位置 | 提供什么 |
|---|---|---|
| `IRQ_S_SOFT` = 1 | `libs/riscv.h` | 软件中断号 |
| `IRQ_S_TIMER` = 5 | `libs/riscv.h` | 时钟中断号 |
| `IRQ_S_EXT` = 9 | `libs/riscv.h` | 外部中断号 |
| `TICK_NUM` = 100 | `kern/trap/trap.c` | 打印周期 |
| `volatile size_t ticks` | `kern/driver/clock.c` + `clock.h` | 全局时钟计数 |
| `clock_set_next_event()` | `kern/driver/clock.c` | 重装下一次定时器 |
| `print_ticks()` | `kern/trap/trap.c` | 打印 `"%d ticks\n"`，即 `100 ticks` |
| `print_trapframe(tf)` | `kern/trap/trap.c` | 打印完整 trapframe |
| `sbi_shutdown()` | `libs/sbi.h` | 关机；**需要自己补 `#include <sbi.h>`** |

[GUARANTEE]

```c
void interrupt_handler(struct trapframe *tf);   // 签名不可改，不许改名、不许加参数
```

[SPECIFICATION]

- **Pre-condition**：`tf` 不是空指针且指向一个有效的 trapframe；`tf->cause` 的最高位为 1（中断）。
- **Post-condition**：中断被处理完毕，函数正常返回；`tf` 中除 `epc` 外的字段不被破坏（本函数不修改 `epc`）；返回后由 `RESTORE_ALL` 与 `sret` 把 CPU 送回被打断处。

- **Case 1（时钟中断）**：`cause & ~(1<<63) == IRQ_S_TIMER`。
  - `clock_set_next_event()` 被调用，使 `ticks` 能持续增长；
  - `ticks` 恰好 +1；
  - 当 `ticks % 100 == 0` 时输出一行 `100 ticks`；
  - 当已输出次数 `ticks / 100 >= SHUTDOWN_AFTER` 时调用 `sbi_shutdown()`，此后再不返回。

- **Case 2（软件中断）**：输出 `Supervisor software interrupt`，不改变任何全局状态。

- **Case 3（外部中断）**：输出分类提示（本实验不做设备处理），不改变任何全局状态。

- **Case 4（未知中断）**：输出 `Unknown interrupt: cause <十进制数>`，并打印 trapframe 便于排查；**不得 panic、不得死循环**，必须返回。

- **边界情况**：
  - `cause = 15`（非法但非负、且没有 bit63）也必须走 `default` 分支打印出 `Unknown interrupt: cause 15`；
  - 不能假设 `tf->cause` 一定是标准中断号，测试会手工构造 trapframe 直接调用本函数；
  - `SHUTDOWN_AFTER` 必须满足 `SHUTDOWN_AFTER * TICK_NUM > 200`，否则会在 Test 3 等到的 `ticks >= 200` 之前关机。

- **不变式**：无论走哪个分支（关机分支除外），函数都必须在有限步内返回。

### 生成测试的提示词

[PROMPT]
不要另写测试文件，用课程自带的 `make grade` 验收。读 `tools/grade.sh` 与 `tools/test_lab3.c`，弄清与本模块相关的三个断言，然后只根据串口输出判断：

1. Test 2 里 `test_lab3_handler_fallbacks()` 手工构造一个全零 trapframe，令 `cause = IRQ_S_SOFT | (1<<63)` 后直接调用 `interrupt_handler`，要求输出包含 `Supervisor software interrupt`；
2. 同一个函数再把 `cause` 置为 `15`，要求输出包含 `Unknown interrupt: cause 15`；
3. Test 3 在 `intr_enable()` 之后忙等 `ticks >= 200`，要求输出包含 `100 ticks` 与 `ticks=200`，并最终打印 `[Test 3] Sustained supervisor timer: PASS`。

[GUARANTEE]
测试只依赖两项外部可观察量：串口输出字符串（`grep -F` 精确匹配）与测试代码自己的返回值，不读内核内部变量（`ticks` 除外，那是测试自己在等）。

[SPECIFICATION]

- **通过条件（Test 3，40 分）**：同时命中 `[Test 3] Sustained supervisor timer: PASS`、`100 ticks`、`ticks=200`。
- **通过条件（Test 2 中属于本模块的部分）**：同时命中 `Supervisor software interrupt` 与 `Unknown interrupt: cause 15`（Test 2 的另外两条 `Illegal instruction at`、`cause    0x00000014` 属于异常处理分支）。
- **失败形态 1**：只打出 1 行 `100 ticks` → 说明没有重装定时器。
- **失败形态 2**：完全没有任何中断输出 → 说明 `case` 没命中（bit63 没掩）或 Trap 根本回不来。
- **失败形态 3**：字符串差一个空格、或把 `cause 15` 打成十六进制 `0xf` → `grep -F` 不匹配。

### 测试覆盖说明

| 覆盖点 | 测试手段 | 期望结果 |
|---|---|---|
| 中断号掩码 | 构造 `cause = IRQ_S_SOFT \| (1<<63)` | 命中软件中断分支（若没掩 bit63 会落到 default） |
| 软件中断分支 | 同上 | `Supervisor software interrupt` |
| 未知中断兜底 | 构造 `cause = 15` | `Unknown interrupt: cause 15` |
| 定时器重装 | Test 3 等 `ticks >= 200` | 40 秒内 `ticks` 持续增长 |
| 打印周期 | `ticks % TICK_NUM == 0` | `100 ticks` 连续出现 10 行 |
| 关机时机 | `SHUTDOWN_AFTER = 10` | 1000 ticks 才关机，测试先完成 |
| 不破坏调用者状态 | Test 2 手工 trapframe 直接调用 | 函数正常返回，不 panic |

**实测结果（2026-10-29）**：Test 2 `PASS (25/25)`、Test 3 `PASS (40/40)`；Test 1 属功能模块一，当前 0/35，与本节无关。
