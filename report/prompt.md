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

### 生成测试的提示词

### 测试覆盖说明

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
