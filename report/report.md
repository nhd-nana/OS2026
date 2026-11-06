# 操作系统实验报告

## 实验基本信息

| 项目 | 内容 |
|---|---|
| 实验名称 | Lab 3：中断与异常 |
| 小组成员 | 2412799-葛熠（队长）、2412133-吕鹏哲、2412679-钟一成 |
| 完成日期 | 2026-10-29 |

### 小组分工

| 成员 | 负责的模块 |
|---|---|
| 2412799-葛熠 | lab2 代码回填、环境与构建、`exception_handler()`、报告统稿 |
| 2412133-吕鹏哲 | 功能模块一 `trapentry.S`（`RESTORE_ALL`、`__alltraps`） |
| 2412679-钟一成 | 功能模块二 `interrupt_handler()` |

## 一、实验目的

Lab1 让内核"能启动、能打印"，Lab2 让内核"能管理物理内存"。这两个阶段的内核都是**一条直线跑到底**：CPU 从 `kern_init()` 开始顺序执行，没有任何外部力量能让它中途停下来。

Lab3 要补上的正是这个能力——**让内核能被外部事件打断，处理完再原样回到被打断的地方继续跑**。具体目标：

1. 理解 RISC-V 的 Trap 机制：`stvec`、`sepc`、`scause`、`sstatus`、`sscratch` 这几个 CSR 各自负责什么；
2. 手写汇编完成**上下文保存与恢复**，理解为什么 `sp` 必须特殊处理；
3. 用 C 完成**中断与异常的分类处理**，特别是时钟中断的"重装"语义；
4. 理解"异常处理必须推进 `epc`"这一条看似琐碎、实则决定内核会不会死循环的规则。

## 二、实验环境

| 项 | 值 |
|---|---|
| 宿主机 | Windows 11 + WSL2（Ubuntu 24.04.4 LTS） |
| 交叉工具链 | `riscv64-unknown-elf-gcc 15.1.0`、GDB 16.3 |
| 模拟器 | 本地编译的 **QEMU 4.1.1**（`~/opt/qemu-4.1.1/bin/qemu-system-riscv64`），自带 OpenSBI v0.4 |
| 构建命令 | 在 `<仓库根>/code` 下 `make`、`make qemu`、`make grade` |

**两处工具链兼容修复**（课程材料按更老的 binutils / 头文件写的，不改根本编不过）：

| 报错 | 原因 | 改法 |
|---|---|---|
| `trapentry.S: Error: unknown CSR 'sbadaddr'` | binutils 2.45 删除了旧别名，只认 `stval`（同一个 CSR `0x143`） | 文件顶部 `#define sbadaddr stval`，`SAVE_ALL` 指令序列保持与指导书一致 |
| `pmm.c: 'PHYSICAL_MEMORY_END' undeclared` | lab3 的 `memlayout.h` 没有这个宏（lab2 是我们自己加的） | `memlayout.h` 补 `#define PHYSICAL_MEMORY_END 0x88000000` |

**为什么仍然用 QEMU 4.1.1**：沿用 Lab2 的结论。lab3 实测 4.1.1 下 `-device loader,addr=0x80200000` 与 `-kernel bin/ucore.img` 两种写法都能正常启动，因此 lab3 的 `Makefile` 与 `tools/grade.sh` 不需要改动（lab2 之所以改成 `-kernel`，是因为那边被迫用了 QEMU 7 的新版 OpenSBI，两者的 `satp` 翻译时序不同）。

## 三、实验整体逻辑分析

### 3.1 本章节的逻辑主线

Lab3 的全部代码都围绕**一条链**展开，任何一个环节断了，内核的表现都是"卡住"或"反复重启"：

```text
时钟/异常发生
   │
   ├─ CPU 硬件：把断点存进 sepc、原因存进 scause、原 sstatus 存进 sstatus.SPIE，跳 stvec
   ▼
__alltraps  →  SAVE_ALL：在栈上构造 trapframe（36 个槽位）
   │
   ▼
mv a0, sp; jal trap  →  trap(tf)
   │
   ├─ trap_dispatch：tf->cause 最高位为 1 → 中断；为 0 → 异常
   │        ├─ interrupt_handler(tf)：时钟 / 软件 / 外部 / 未知
   │        └─ exception_handler(tf)：非法指令 / 断点 / 兜底
   ▼
__trapret  →  RESTORE_ALL：从 trapframe 反向恢复 36 个槽位
   │
   ▼
sret  →  CPU 用 sstatus.SPP 决定回哪一级、用 sepc 决定回哪条指令
```

这条链上有两个"必须成对"的设计，是本章的核心：

- **`SAVE_ALL` 与 `RESTORE_ALL` 必须严格互逆**：一个按什么顺序存，另一个就得按相反的顺序取，否则取到的是别的寄存器；
- **`trap` 与 `sret` 必须严格配对**：`trap` 里改了 `tf->epc`，`sret` 才会跳到新地址；不推进 `epc`，`sret` 会回到同一条指令，异常无限重现。

### 3.2 功能的逐步实现

我们按"能编译 → 能进 Trap → 能回来 → 能处理"四步推进：

1. **环境与 lab2 回填**（葛熠）。lab3 材料里的 `kern/mm` 是空骨架，`pmm.c` 只剩 `//lab2 请补充你在lab2中的代码`。不把 `page_init()` 和 Best-Fit 搬回来，`npage`、`pages[]`、空闲链表全是空的，内核连内存都摸不到，后面什么都谈不上。
2. **读懂 trapframe 的布局**。`struct pushregs` 的字段顺序恰好是 RISC-V 的 `x0..x31` 编号顺序，`struct trapframe` 再追加 4 个 CSR 槽位，一共 36 个 `uintptr_t`。这一步的意义在于：`SAVE_ALL` 里 `STORE xN, N*REGBYTES(sp)` 的偏移量与 C 结构体字段一一对应，后面查 `tf->gpr.t0` 时才不会错位。
3. **写 `RESTORE_ALL`**（模块一）。它是 `SAVE_ALL` 的镜像，难点集中在 `sp` 和 4 个 CSR 上。
4. **接上 `jal trap`**。`__alltraps` 保存完上下文后，按 RISC-V 调用约定把 `sp` 放进 `a0` 再 `jal trap`，C 代码才拿得到 `tf`。
5. **写两个 handler**（模块二）。到这一步内核已经能进能出，剩下的就是"进来之后干什么"：中断按中断号分发，异常按异常号分发。

## 四、实验内容与实现

### 功能模块一：Trap 入口与上下文恢复

**负责人：** 2412133-吕鹏哲

**一、模块功能描述**

本模块负责 Trap 从"进入内核"到"返回原执行流"之间的底层上下文管理，对应 `kern/trap/trapentry.S` 里的两个汇编符号，合起来补上"保存 → 处理 → 恢复"这条闭环的另外两段：

```text
Trap 发生 → __alltraps 建立入口 → SAVE_ALL 保存上下文（已给）
          → mv a0, sp; jal trap 把 tf 交给 C 层 trap(tf) 处理
          → __trapret → RESTORE_ALL 恢复上下文（本模块）→ sret 返回
```

- **`RESTORE_ALL`**：`SAVE_ALL` 的镜像。`SAVE_ALL` 把 Trap 发生时的处理器状态（x0..x31 + `sstatus`/`sepc`/`badvaddr`/`cause` 共 36 个槽位）保存到当前内核栈上的 trapframe，`RESTORE_ALL` 按完全相同的布局反向取回，使紧跟其后的 `sret` 能回到被打断处。
- **`__alltraps`**：汇编 Trap 入口。`SAVE_ALL` 之后把 trapframe 指针按 RISC-V 调用约定放进第一参数寄存器 `a0`，再 `jal trap` 调用 `trap(struct trapframe *tf)`，把保存好的上下文交给 C 层分类处理。

整个模块的对外接口链是 `__alltraps → trap(struct trapframe *tf) → __trapret → sret`，本模块不修改 `SAVE_ALL`、`struct trapframe`、`trap()` 签名。

**二、实现**

`RESTORE_ALL`（`__trapret` 里 `sret` 之前调用）：

```asm
.macro RESTORE_ALL
    # 先取两个 CSR 槽位：RISC-V 不能从内存直写 CSR，必须经 GPR 中转
    LOAD s1, 32*REGBYTES(sp)        # s1 = 保存的 sstatus
    LOAD s2, 33*REGBYTES(sp)        # s2 = 保存的 sepc

    # 用 SSTATUS_SPP 判断返回特权级：SPP=1 来自内核态，sscratch 保持 0；
    # SPP=0 来自用户态（lab4 用到），先把内核 trapframe 指针存回 sscratch
    andi s0, s1, SSTATUS_SPP
    bnez s0, 1f
    csrw sscratch, sp
1:
    csrw sstatus, s1
    csrw sepc, s2

    # 恢复 x1, x3..x31（跳过硬连线为 0 的 x0）
    LOAD x1, 1*REGBYTES(sp)
    LOAD x3, 3*REGBYTES(sp)
    # …… x4 .. x31 逐行，偏移 = 寄存器号 * REGBYTES ……
    LOAD x31, 31*REGBYTES(sp)
    # sp 必须最后恢复：它是上面所有 LOAD 的基址
    LOAD x2, 2*REGBYTES(sp)
.endm
```

`__alltraps`：

```asm
__alltraps:
    SAVE_ALL
    mv a0, sp          # a0 = trapframe 指针（RISC-V 第一参数）
    jal trap           # 调 C 函数 trap(tf)
    # trap 是 C 函数，sp 属 callee-saved，返回后 sp 不变，落到 __trapret
```

完整逐行代码见 `kern/trap/trapentry.S`（x3..x31 的 `LOAD` 全部展开，偏移与 `SAVE_ALL` 的 `STORE` 一一对应）。

**三、最终提示词**

与 `report/prompt.md` 功能模块一的「生成实现的提示词」逐字一致，四段式 `[PROMPT]`/`[RELY]`/`[GUARANTEE]`/`[SPECIFICATION]`：

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

**四、实现迭代过程**

**第 1 次生成 → Test 1 未过（65/100）。** 按提示词生成 `RESTORE_ALL` 与 `__alltraps` 后 `make grade`：

```text
  - trap entry, register restore, and breakpoint EPC: FAIL (0/35)
  - illegal instruction EPC advance and return: PASS (25/25)
  - re-armed supervisor timer and repeated trap return: PASS (40/40)
Total Score: 65/100
```

现象：`make qemu`/串口能看到 `Breakpoint at 0xffffffffc02019c8`，随后打印 `[Test 1] Trap entry and breakpoint return: FAIL (register context)`。说明 Trap 能进、能出、`exception_handler` 也推进了 epc，但 26 个寄存器比对不过。

**排查：反汇编测试函数定位错位。** `riscv64-unknown-elf-objdump -d bin/kernel` 看 `test_lab3_register_context`：

```text
ffffffffc02019c8:	9002                	ebreak
ffffffffc02019ca:	10500513          	li	a0,261
ffffffffc02019ce:	0ca29863          	bne	t0,a0,...
```

问题就在这里：`ebreak` 是 **2 字节**的 `c.ebreak`（编码 `0x9002`），而 `exception_handler` 按规范对断点做 `tf->epc += 4`。`epc` 从 `0x…19c8` 加 4 落到 `0x…19cc`，正好落在 `li a0,261`（4 字节，`0x…19ca`~`0x…19cd`）的**中间**，`sret` 从半条指令处继续执行，寄存器自然乱掉。

**根因：压缩指令扩展。** 工具链默认 `-march=rv64imafdc`（末尾的 `c` 即压缩扩展），会把 `ebreak` 压缩成 2 字节。单独验证：

```text
$ printf "ebreak\n" | as -march=rv64imafdc -o /tmp/a.o -   # → 9002  ebreak（2 字节）
$ printf "ebreak\n" | as -march=rv64imafd  -o /tmp/b.o -   # → 00100073  ebreak（4 字节）
```

而本实验规范里异常处理统一 `epc += 4`、非法指令 `.word 0x0000007b` 也是显式 4 字节，即**规范假设所有异常指令都是 4 字节**。去掉压缩扩展就能恢复这个假设。

**修复：`Makefile` 显式去掉 `c`。** 在 `CFLAGS` 加 `-march=rv64imafd`（与团队已做的 `#define sbadaddr stval`、补 `PHYSICAL_MEMORY_END` 属同类"工具链/材料跟不上环境"的兼容修复）：

```makefile
CFLAGS  := -mcmodel=medany -march=rv64imafd -std=gnu99 -Wno-unused -Werror
```

**修复后 `make grade`：**

```text
  - trap entry, register restore, and breakpoint EPC: PASS (35/35)
  - illegal instruction EPC advance and return: PASS (25/25)
  - re-armed supervisor timer and repeated trap return: PASS (40/40)
Total Score: 100/100
```

> 备注：本机本次实际用的工具链是 `riscv64-unknown-elf-gcc (SiFive GCC-Metal 10.2.0-2020.12.8) 10.2.0`（位于 `/mnt/d/OS2026/riscv/…`，因环境里没有文档所述的 15.1.0）。`-march=rv64imafd` 对任何版本都无害：它只是禁用压缩指令，让 `ebreak` 恒为 4 字节，正好符合规范对 `epc += 4` 的假设。

**统稿补充（2026-11-07，葛熠）：把"固定 +4"升级为"按指令编码判断长度"。**

上面 `-march` 的做法能让本实验通过，但规范的原文要求"不同异常对应的指令长度可能不同，不能假定所有异常都使用相同的 epc 偏移量，需要结合 RISC-V 指令编码规则判断"。只靠禁用压缩扩展，等于把这个问题绕过去：一旦今后开启 C 扩展（例如 lab4 的用户态程序按默认 `-march` 编译），固定 `+4` 会再次踩坑。

因此 `exception_handler` 现在改为读触发异常那条指令的首字节来判断长度：

```c
/* 低 2 位为 0b11 → 32 位指令；否则是 16 位压缩指令 */
static size_t
instruction_len(uintptr_t epc)
{
    uint16_t head = *(volatile uint16_t *)epc;
    return (head & 0x3) == 0x3 ? 4 : 2;
}
```

两处 `tf->epc += 4` 都换成 `tf->epc += instruction_len(tf->epc)`。

验证方式：临时把 `-march=rv64imafd` 从 `CFLAGS` 去掉（此时工具链默认 `rv64imafdc`，`ebreak` 重新变成 2 字节 `c.ebreak`），重新 `make grade`：

```text
  - trap entry, register restore, and breakpoint EPC: PASS (35/35)
  - illegal instruction EPC advance and return: PASS (25/25)
  - re-armed supervisor timer and repeated trap return: PASS (40/40)
Total Score: 100/100
```

即"2 字节断点"这条路径也被真正走通，不再依赖编译选项。`-march=rv64imafd` 作为构建一致性约束保留
（它同时保证内核里不混入压缩指令，便于与 `SAVE_ALL`/trapframe 的机械对照），但**不再是正确性的前提**。

**五、测试覆盖说明**

| 覆盖点 | 测试手段 | 对应实现 | 结果 |
|---|---|---|---|
| Trap 入口 + 传参 | `ebreak` → `__alltraps` | `mv a0, sp` + `jal trap` | `Breakpoint at` 打印 |
| 26 个寄存器恢复 | 赋 0x105..0x11f 后逐位比对 | 逐个 `LOAD x<i>, i*REGBYTES(sp)` | PASS |
| sp 特殊恢复 | `x2` 从槽位 2 最后恢复 | `LOAD x2, 2*REGBYTES(sp)` 放最后 | PASS |
| CSR 恢复 | `sret` 回到 `ebreak` 下一条 | `LOAD s1/s2` + `csrw sstatus/sepc` | PASS |
| 断点 epc 推进 | 4 字节指令 +4 | `instruction_len()` 判 `0b11` | PASS |
| 2 字节断点的 epc 推进 | 去掉 `-march=rv64imafd` 让 `ebreak` 变 `c.ebreak` | `instruction_len()` 返回 2 | PASS |

### 功能模块二：中断与异常处理

**负责人：** 2412679-钟一成（`interrupt_handler`）

**一、入口：中断和异常是怎么被分开的**

`trap(tf)` 本身不做任何判断，真正的分发在 `trap_dispatch()`：

```c
static inline void trap_dispatch(struct trapframe *tf) {
    if ((intptr_t)tf->cause < 0) {
        interrupt_handler(tf);   // 最高位为 1：中断
    } else {
        exception_handler(tf);   // 最高位为 0：异常
    }
}
```

**为什么用"是否有符号 < 0"来判断？** 因为 RISC-V 规定 `scause` 的最高位（bit 63）是"中断标志位"：中断时置 1，异常时为 0。把它当成有符号数看，bit63 就是符号位，`< 0` 恰好等价于"这是中断"。这也提醒我们：`tf->cause` 里**混着标志位和编号**，真正的中断号在低 63 位。

**二、实现**

```c
#define SHUTDOWN_AFTER 10

void interrupt_handler(struct trapframe *tf) {
    uintptr_t cause = tf->cause & ~((uintptr_t)1 << 63);   // 掩掉中断标志位

    switch (cause) {
        case IRQ_S_TIMER:                       // 5：时钟中断
            clock_set_next_event();             // ① 重装下一次
            ticks++;                            // ② 计数
            if (ticks % TICK_NUM == 0) {        // ③ 每 100 次打印
                print_ticks();
                if (ticks / TICK_NUM >= SHUTDOWN_AFTER) {
                    sbi_shutdown();                 // ④ 打印够 10 次后关机
                }
            }
            break;
        case IRQ_S_SOFT:                        // 1：软件中断
            cprintf("Supervisor software interrupt\n");
            break;
        case IRQ_S_EXT:                         // 9：外部中断
            cprintf("Supervisor external interrupt\n");
            break;
        default:
            cprintf("Unknown interrupt: cause %d\n", (int)cause);
            print_trapframe(tf);
            break;
    }
}
```

另外需要在文件头部补 `#include <sbi.h>`（`sbi_shutdown` 的声明）。`trap.c` 原本只包含了 `clock.h`、`riscv.h` 等，没有 `sbi.h`；而编译选项里有 `-Werror`，漏掉这个头文件会以 `implicit declaration of function 'sbi_shutdown'` 直接失败，而不是先跑起来再出问题。

**三、四个分支各自的职责**

| 分支 | 中断号 | 行为 | 为什么必须这样 |
|---|---|---|---|
| 时钟 `IRQ_S_TIMER` | 5 | 重装 → `ticks++` → 每 `TICK_NUM` 打印一次 → 够 `SHUTDOWN_AFTER` 次关机 | `sbi_set_timer` 是**一次性闹钟**，不重装就只有第一次中断 |
| 软件 `IRQ_S_SOFT` | 1 | 打印 `Supervisor software interrupt` | 用于区分中断来源，测试用它验证分发正确 |
| 外部 `IRQ_S_EXT` | 9 | 打印 `Supervisor external interrupt` | 本实验未接 PLIC，只做分类提示，不做设备相关处理 |
| 其他 | — | 打印 `Unknown interrupt: cause <n>` + `print_trapframe` | 未知中断不能静默吞掉，否则问题无从排查 |

**四、三个关键决策**

1. **必须掩掉 bit63 再比较。**
   这是本模块最容易踩的坑。真实硬件产生的 `scause` 是 `0x8000000000000005`（时钟中断），而宏 `IRQ_S_TIMER` 只有 5。如果直接 `switch (tf->cause)`，四个 `case` 一个都不会命中，全部落到 `default`——测试里的 `Supervisor software interrupt`、`Unknown interrupt: cause 15` 都打不出来，Test 2 与 Test 3 直接归零。测试代码里也正是用 `IRQ_S_SOFT | ((uintptr_t)1 << 63)` 来构造"带标志位的中断号"。

2. **`clock_set_next_event()` 必须放在每次中断里。**
   它在 `clock.c` 里是 `sbi_set_timer(get_cycles() + timebase)`，本质是"把定时器设到**从现在开始**再过 `timebase` 个周期触发"，是一次性的。只在 `clock_init()` 里设一次，第一次中断之后定时器就再也不会响，`ticks` 会永远停在 1。Test 3 要求 `ticks` 涨到 200，靠的就是每次中断都重装。

3. **`SHUTDOWN_AFTER` 取 10 而不是 1。**
   `print_ticks()` 每 100 个 tick 打印一次 `100 ticks`。如果打印 1 次就关机，关机发生在 `ticks == 100` 时，而测试还在等 `ticks >= 200`，Test 3 会因为"内核还没跑完就被拔电源"而失败。取 10 意味着 1000 个 tick 之后才关机，既满足测试，又能让 `make qemu` 自己退出，不必每次按 `Ctrl+A` 再按 `X`。

**五、输出字符串必须逐字匹配**

`tools/grade.sh` 用 `grep -F` 精确匹配，多一个空格、换一种进制都不算通过：

| 字符串 | 谁产生 | 备注 |
|---|---|---|
| `Supervisor software interrupt` | `interrupt_handler` | Test 2 要求 |
| `Unknown interrupt: cause 15` | `interrupt_handler` | Test 2 要求；`%d` 十进制 |
| `100 ticks` | 框架 `print_ticks()`，由我调用 | Test 3 要求；不要改格式 |
| `cause    0x00000014` | 框架 `print_trapframe()` | 用于异常兜底路径；冒号后 **4 个空格**，`%08x` 共 8 位 |

**六、`ticks` 的生命周期**

`ticks` 定义在 `clock.c`（`volatile size_t ticks;`），`clock_init()` 里在 `clock_set_next_event()` 之后清零。每次时钟中断 +1，每满 100 由 `print_ticks()` 打印一次 `100 ticks`。测试在 `intr_enable()` 之后忙等 `ticks >= 200`，所以只要有一点"没重装"或"分发没命中"，这个循环就永远出不来，QEMU 会在 30 秒后超时——这也是 Test 3 最容易暴露问题的地方。

### 思考题

**思考题：`local_intr_save` 为什么用 `do { } while (0)` 包一层**

`kern/sync/sync.h` 里的定义是：

```c
static inline bool __intr_save(void) {
    if (read_csr(sstatus) & SSTATUS_SIE) {
        intr_disable();
        return 1;
    }
    return 0;
}

#define local_intr_save(x) \
    do {                   \
        x = __intr_save(); \
    } while (0)
```

它做的事情是：**如果当前开着中断，就关掉，并把"原来开着"这个事实记进 `x`**；配合 `local_intr_restore(x)` 使用，后者根据 `x` 决定要不要把中断重新打开。这样一段临界区无论进出时中断是什么状态，退出后都恢复到原来的状态——这正是"可嵌套"的关键：内层临界区结束时不会误开外层仍在保护的中断。

至于为什么要用 `do { } while (0)` 包一层，有三个原因：

1. **让宏在语法上"表现得像一个函数调用"。** 宏只是文本替换。如果写成 `#define local_intr_save(x) x = __intr_save()`，一旦宏体里有不止一条语句，下面这种写法就会出错：

   ```c
   if (条件)
       local_intr_save(flag);   // 只有第一条语句受 if 管辖，后面的无条件执行
       do_something();
   ```

   `do { ... } while (0)` 把整个宏体包成**一条**复合语句，后面跟分号（`do{...}while(0);`），既能放进 `if/else`，也能放进 `for`、`switch`，行为与函数调用完全一致。

2. **避免 `if` 悬空（dangling else）。** 直接用 `{ }` 包一层也不行：

   ```c
   if (a)
       { ... };      // 这个分号是一条空语句
   else
       ...           // 于是 else 找不到匹配的 if，编译报错
   ```

   `do{...}while(0)` 自带一个"完整语句 + 分号"的形态，恰好满足 C 语法对语句的要求。

3. **提醒使用者这是一段不可分割的临界区。** `local_intr_save(x)` 与 `local_intr_restore(x)` 必须成对出现，中间不能有 `break`/`continue`/`return` 漏过，也不能提前释放。`do{}while(0)` 的块结构在阅读时就是一道视觉边界。

顺带一提：`local_intr_restore(x)` 的定义结尾带了分号（`#define local_intr_restore(x) __intr_restore(x);`），这其实是个小瑕疵——在 `if (c) local_intr_restore(x); else ...` 里会多出一个空语句让 `else` 失配。规范写法同样应该用 `do{}while(0)` 包起来。

## 五、测试与验证

### 5.1 运行结果（`make qemu`）

```bash
cd <仓库根>/code
make
make qemu
```

串口实际输出（2026-11-07 复验，QEMU 4.1.1 + gcc 15.1.0）：

```text
OpenSBI v0.4 (Jul  2 2019 11:53:53)
...
DTB Init
HartID: 0
DTB Address: 0x82200000
Physical Memory from DTB:
  Base: 0x0000000080000000
  Size: 0x0000000008000000 (128 MB)
  End:  0x0000000087ffffff
DTB init completed
(THU.CST) os is loading ...
Special kernel symbols:
  entry  0xffffffffc020005c (virtual)
  etext  0xffffffffc0202804 (virtual)
  edata  0xffffffffc0207028 (virtual)
  end    0xffffffffc0207498 (virtual)
Kernel executable memory footprint: 30KB
memory management: best_fit_pmm_manager
phys memory: [0x0000000080000000, 0x0000000088000000)
npage: 0x88000
free pages: 0x7cb8
check_alloc_page() succeeded!
satp virtual address: 0xffffffffc0206000
satp physical address: 0x0000000080206000
++ setup timer interrupts
100 ticks
100 ticks
100 ticks
100 ticks
100 ticks
100 ticks
100 ticks
100 ticks
100 ticks
100 ticks
```

怎么读这段输出：

- `++ setup timer interrupts` 是 `clock_init()` 打的，说明 `sie.STIP` 已使能、第一次闹钟已设好；
- 随后每 100 个 tick 打一行 `100 ticks`（`TICK_NUM = 100`），**能连续打出 10 行而不是 1 行**，直接证明"每次中断都重装了定时器"；
- 打完第 10 行后内核调用 `sbi_shutdown()`，QEMU 自行退出，无需 `Ctrl+A` `X`。

### 5.2 自动评分（`make grade`）

```bash
make grade
```

`make grade` 会：把工作区复制到 `/tmp/lab3_grade` → `inject.py` 注入 `kern/test/` 三个测试源并在 `kern_init()` 里插入调用 → 用不同 `-D` 宏编译三次、分别跑三次 QEMU → 用 `grep -F` 检查串口输出。

当前实测结果（2026-11-07 复验，gcc 15.1.0 + QEMU 4.1.1；模块一合入后的完整结果）：

```text
  - trap entry, register restore, and breakpoint EPC: PASS (35/35)
  - illegal instruction EPC advance and return: PASS (25/25)
  - re-armed supervisor timer and repeated trap return: PASS (40/40)
Total Score: 100/100
```

| 测试 | 分值 | 结果 | 与本人的关系 |
|---|---|---|---|
| Test 1 Trap 入口 + 寄存器恢复 + 断点返回 | 35 | ✅ PASS | 功能模块一，非本人负责（吕鹏哲实现） |
| Test 2 非法指令 EPC 前进 + 返回 | 25 | ✅ PASS | 其中的软件中断与未知中断分支由本人实现 |
| Test 3 时钟重装 + 反复 trap 返回 | 40 | ✅ PASS | 全部由本人实现 |

> 时间线说明：模块二单独提测时（2026-10-29）只有 Test 2/3 通过，Test 1 因 `trapentry.S` 尚未实现而 FAIL，得分 65/100；
> 模块一合入后为 100/100。下表的覆盖分析针对 `interrupt_handler` 本身，与模块一是否合入无关。

**Test 2 里属于本人的部分**：25 分要求同时命中四个字符串——`Illegal instruction at`、`Supervisor software interrupt`、`Unknown interrupt: cause 15`、`cause    0x00000014`。中间两条正是 `interrupt_handler` 的软件中断分支与未知中断分支打印的，它们能同时出现在输出里，说明"掩 bit63 → 分发 → 分类打印"这条路径是通的。

**Test 3 全部属于本人**：40 分要求串口里出现 `100 ticks` 和测试自己打印的 `ticks=200`，也就是 `ticks` 必须真的从 0 涨到 200 以上。这只有"每次时钟中断都重装定时器"才做得到，是"一次性闹钟"这个坑的直接证据。

**测试覆盖说明**：

| 覆盖点 | 手段 | 对应实现 |
|---|---|---|
| 中断号掩码是否正确 | 构造 `cause = IRQ_S_SOFT \| (1<<63)` 调 handler | `tf->cause & ~(1UL<<63)` |
| 软件中断分支 | 同上 | `Supervisor software interrupt` |
| 未知中断兜底 | 构造 `cause = 15` | `Unknown interrupt: cause 15` |
| 定时器能否持续重装 | `kern_init` 末尾忙等 `ticks >= 200` | `clock_set_next_event()` |
| 打印周期是否正确 | `ticks % TICK_NUM == 0` | `100 ticks` 连续 10 行 |
| 关机时机不影响测试 | `SHUTDOWN_AFTER = 10` | 测试先于关机完成 |

这些测试是**黑盒**的：它们只看串口输出里的字符串，不直接读内核内部变量（`ticks` 除外，那是测试自己等它涨）。

## 六、实验总结与收获

### 6.1 本实验知识点与 OS 原理的对应

| 实验里做的事 | OS 原理中的概念 | 关系说明 |
|---|---|---|
| `stvec` 指向 `__alltraps` | 中断向量表 / 异常入口 | CPU 硬件只负责"跳到一个固定地址"，具体保存什么、怎么分发，全由软件设计 |
| `SAVE_ALL` 构造 trapframe | 上下文（context） | 进程切换保存的也是同一套东西；trapframe 是"一次进入内核"的最小上下文 |
| `sret` 用 `sepc`/`sstatus.SPP` 返回 | 特权级切换、精确异常 | 硬件保证"异常发生时 `sepc` 精确指向出错指令"，软件必须自己决定要不要跳过它 |
| `tf->epc += 4` | 异常恢复语义 | 决定了"异常是可恢复的还是致命的"——不推进就是死循环 |
| 时钟中断重装 | 定时器 / 时间片 | 抢占式调度的时间基准；没有周期性时钟，就没有"抢占" |
| `interrupt_handler` 按 `cause` 分发 | 中断子系统 | 真实 OS 里这一层会再分到 PLIC/驱动，形成中断上半部 |
| `local_intr_save/restore` | 临界区保护 | 关中断是最朴素的自旋锁替代品，用于保护 `pmm` 的空闲链表这类共享结构 |

### 6.2 OS 原理中重要但本实验未覆盖的知识点

1. **中断嵌套与优先级**：本实验在 `interrupt_handler` 里没有重新开中断，所有中断是串行处理的。真实内核要区分可嵌套/不可嵌套中断，并用优先级避免低优先级中断长期饿死。
2. **外部中断的完整链路**：`IRQ_S_EXT` 只打印了一句话。真实系统要通过 PLIC 读 `claim` 寄存器拿到中断源编号，再分发给对应驱动，处理完还要 `complete`。
3. **中断上半部 / 下半部**：硬中断上下文里不能睡眠、不能长时间占用，真实内核把耗时工作推迟到软中断、tasklet 或工作队列。
4. **缺页异常与按需分页**：本实验的异常处理只有非法指令和断点。Lab5 才会引入 page fault，届时 `badvaddr`（本实验只保存不使用）才真正派上用场。
5. **多核与 IPI**：`sbi_send_ipi` 在 `sbi.h` 里存在但没用到；SMP 下时钟中断、TLB shootdown 都要靠 IPI。
6. **用户态与系统调用**：本实验全程在 S 态，U 态切换（Challenge）只做了原理说明。

### 6.3 AI 协作开发的经历

1. **提示词要把"不可协商的约束"前置**。比如"不得修改 `SAVE_ALL`、`struct trapframe`、`trap()` 签名""中断号必须先掩掉 bit63""输出字符串必须逐字一致"。这些约束一旦漏写，生成的代码往往"能跑但过不了测试"，返工成本比一开始写清楚高得多。
2. **让 AI 先读测试再写实现**。`tools/grade.sh` 和 `tools/test_lab3.c` 里写死了评分标准（grep 的字符串、构造的 `cause` 值）。先让 AI 把这些读出来，再让它写 `interrupt_handler`，可以避免"实现对了但字符串差一个空格"这种低级失分。
3. **AI 生成的汇编必须人工逐条核对**。`RESTORE_ALL` 这种代码，编译器不会报错，跑起来也可能"看起来正常"，只有在 `make grade` 的寄存器比对下才会暴露。我们因此坚持"每一版都跑 `make grade`，不看 `make qemu` 的观感"。
4. **验收必须是可复现的命令，不是"AI 说通过了"**。本报告的每一个 PASS 都来自实际执行的 `make grade` 输出，而不是生成代码时的自我判断。

## 七、附录：提交记录

| 提交 | 内容 |
|---|---|
| `ec4e2a5` | 建分支、lab2 回填、环境打通 |
| `cf64711` | 三份项目文档入库、实验步骤框架 |
| `be15291` | 实现 `interrupt_handler`（时钟重装 / ticks / 关机 / 中断分类） |
| `92b437b` | 按模板填写模块二报告与提示词 |
| `6144af9` | 实现 `trapentry.S`（`RESTORE_ALL` / `__alltraps`），修复 `c.ebreak` 并补模块一报告 |

**已完成的交付**

- [x] 功能模块一 `RESTORE_ALL` 与 `jal trap`（吕鹏哲）→ Test 1 `PASS (35/35)`
- [x] 功能模块二 `interrupt_handler`（钟一成）→ Test 2 `PASS (25/25)`、Test 3 `PASS (40/40)`
- [x] 功能模块二 `exception_handler`（葛熠）→ 非法指令与断点 epc 前进
- [x] lab2 代码回填与工具链兼容修复
- [x] `make grade` = **100/100**
- [ ] 补充 `make qemu` / `make grade` 截图到 `report/images/`（需人工截图后提交）
