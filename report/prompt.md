# Lab4 提示词记录

每个模块按四段式记录实际用过的提示词：

```
[PROMPT]        任务与操作要求
[RELY]          可依赖的宏 / 结构 / 函数
[GUARANTEE]     必须实现的接口
[SPECIFICATION] Pre/Post-Condition 与 Case
```

---

## 功能模块二：进程创建及初始化（`proc.c`）

**负责**：2412679-钟一成

### 生成实现的提示词

[PROMPT]
在 uCore Lab4 的 `kern/process/proc.c` 中，替换 `//LAB4 function2 YOUR CODE` 标记下的占位实现，补齐 `alloc_proc`、`copy_mm`、`copy_thread`、`do_fork`、`kernel_thread`、`proc_init` 六个函数。要求：

1. `alloc_proc` 用 `kmalloc` 申请 PCB 后**逐字段**初始化，`context` 用 `memset` 清零，`list_link`/`hash_link` 用 `list_init`；
2. `copy_mm` 本实验占位，只把 `proc->mm` 置 `NULL` 并返回 0；
3. `copy_thread` 把 trapframe 模板拷到新内核栈顶，`gpr.a0` 置 0，设 `context.ra = forkret`、`context.sp = proc->tf`；
4. `do_fork` 按 7 步实现，**前 3 步失败必须逐级回滚**（`put_kstack` / `kfree`）；
5. `kernel_thread` 在栈上构造 trapframe 模板并调用 `do_fork`；
6. `proc_init` 造 idleproc 与 initproc，末尾把 `current` 指向 idleproc；
7. **不得修改** `forkret`、`hash_proc`、`find_proc`、`setup_kstack`、`put_kstack`、`get_pid`、`init_main`、`do_exit`，也不得改 `proc_run` / `schedule` / `switch_to`（属于功能模块三）。

[RELY]

| 依赖 | 位置 | 提供什么 |
|---|---|---|
| `struct proc_struct` / `struct context` | `proc.h` | PCB 的 14 个字段；context 只有 `ra/sp/s0..s11` 14 个 callee-saved |
| `bootstack` | `pmm.h:180` | 开机栈基址，idleproc 直接复用 |
| `boot_pgdir_pa` | `pmm.h:33` | 内核页表物理地址，PCB 的 `pgdir` 初值 |
| `KSTACKSIZE` / `KSTACKPAGE` | `memlayout.h` | 内核栈 2 页 = 8KB |
| `SSTATUS_SPP` / `SPIE` / `SIE` | `riscv.h` | 构造 `tf.status` |
| `read_csr` | `riscv.h` | 读当前 `sstatus` |
| `CLONE_VM` | `pmm.h:11` | `kernel_thread` 传 `clone_flags \| CLONE_VM` |
| `get_pid` / `hash_proc` / `find_proc` / `setup_kstack` / `put_kstack` / `forkret` / `wakeup_proc` | `proc.c` / `sched.c` | 已实现，直接调用 |
| `local_intr_save/restore` | `sync/sync.h` | 挂链表时关中断保护 |

[GUARANTEE]

```c
static struct proc_struct *alloc_proc(void);
static int  copy_mm(uint32_t clone_flags, struct proc_struct *proc);
static void copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf);
int         do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf);
int         kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags);
void        proc_init(void);
```

签名不可改：`kernel_thread` 的返回值和参数列表被 `tools/test_lab4.c` 直接调用。

[SPECIFICATION]

- **`alloc_proc`**：成功返回 `state == PROC_UNINIT`、`pid == -1`、`tf == NULL`、`context` 全 0 的 PCB；`kmalloc` 失败返回 `NULL`。
- **`do_fork` Pre**：`tf` 非空（`kernel_thread` 保证）。
- **`do_fork` Post**：成功返回 `pid > 0`，进程已挂进 `proc_list` 与 `hash_list`、状态为 `PROC_RUNNABLE`；失败返回 `-E_NO_MEM` 且**不泄漏**内核栈与 PCB。
- **Case 1（内核线程首跑）**：`copy_thread` 后必须满足 `tf->epc == kernel_thread_entry`、`tf->gpr.s0 == fn`、`tf->gpr.s1 == arg`、`context.ra == forkret`、`context.sp == proc->tf`，四者缺一则"切过去就崩"。
- **Case 2（idleproc）**：`pid == 0`、`state == PROC_RUNNABLE`、`kstack == bootstack`、`need_resched == 1`、`parent == NULL`、`mm == NULL`、`tf == NULL`、`flags == 0`、`context.ra == 0`。
- **Case 3（initproc）**：`pid == 1`、`parent == idleproc`、`mm == NULL`、`context.ra != 0 && context.sp != 0`。
- **边界**：`alloc_proc` 失败时 `do_fork` 直接返回而不回滚（还没有资源）；`setup_kstack` 失败只 `kfree(proc)`；`copy_mm` 失败要 `put_kstack` 再 `kfree`。

### 生成测试的提示词

[PROMPT]
不另写测试。用课程自带的 `make grade` 验收：`tools/inject.py` 会把 `test_lab4_process_create()` 注入到 `proc_init()` 末尾，只要串口里出现 `[Test Process Create] PASS` 即得 10 分。请先读 `tools/test_lab4.c`，把它的每一条 `assert` 当作规格来实现，而不是先写代码再猜测试。

[GUARANTEE]
测试只看串口字符串与 `assert` 是否触发，不读内核内部结构。

[SPECIFICATION]

- 通过条件：输出包含 `[Test Process Create] PASS`。
- 失败形态 1：`assert(current == idleproc)` 失败 → `proc_init` 没有设置 `current`。
- 失败形态 2：`assert(idleproc->kstack == (uintptr_t)bootstack)` 失败 → 用了 `bootstacktop` 或 NULL。
- 失败形态 3：`assert(initproc->context.ra != 0)` 失败 → `copy_thread` 没设 `context.ra`。
- 失败形态 4：`assert(initproc->parent == idleproc)` 失败 → `kernel_thread` 调用前 `current` 还不是 idleproc。

### 测试覆盖说明

| 覆盖点 | 测试手段 | 实测 |
|---|---|---|
| PCB 字段是否全部初始化 | 逐字段 `assert`（`tf`、`runs`、`flags`、`context.ra`） | ✅ |
| idleproc 的栈与状态 | `kstack == bootstack`、`state == PROC_RUNNABLE` | ✅ |
| initproc 是否被正确创建 | `pid == 1`、`parent == idleproc` | ✅ |
| 首次运行的切换目标 | `context.ra != 0 && context.sp != 0` | ✅ |
| 整链是否真的能跑 | `check Schedule`（需功能模块三） | ⏳ 待吕鹏哲合入 |

实测（2026-11-15）：

```text
  -check PMM:                                OK
  -check Process Create:                     OK
  -check Schedule:                           WRONG
Total Score: 20/30
```

---

## 功能模块三：进程调度与上下文切换（`proc.c` / `sched.c` / `switch.S`）

**负责**：2412133-吕鹏哲

### 生成实现的提示词

[PROMPT]
实现内核线程真正交给 CPU 执行的调度与上下文切换模块，使系统不仅能创建 `proc_struct` 线程对象，还能让 `initproc` 从 `idleproc` 手中接管 CPU。核心闭环：`cpu_idle()` 空闲等待调度需求 → `schedule()` 选择下一个可运行线程 → `proc_run()` 更新 current、切换页表并触发底层切换 → `switch_to()` 保存旧线程寄存器现场并恢复新线程寄存器现场。

改哪里：
1. `kern/process/proc.c`：`cpu_idle`、`proc_run`
2. `kern/schedule/sched.c`：`schedule`
3. `kern/process/switch.s`：`switch_to`

怎么改：
- `cpu_idle()` 作为空闲线程执行主体持续循环；检测到 `need_resched` 时调用 `schedule()`，自身不直接选线程、不直接调 `proc_run()`/`switch_to()`。
- `schedule()` 在关中断临界区内从进程链表选出 `PROC_RUNNABLE` 线程；找不到退回 `idleproc`；清理调度标志后调 `proc_run(next)`。
- `proc_run()` 在关中断临界区内保存旧 `current`、更新 `current`、按 Sv39 写 `satp` + `flush_tlb()`，调 `switch_to(&prev->context, &proc->context)`；**不得用 `lsatp()`**（Sv32 遗留），参照 `pmm.c::enable_paging()`。
- `switch_to()` 仅保存/恢复 `ra、sp、s0~s11`，`ret` 跳到 `to->ra`（首跑即 `forkret`）。
- 用 `local_intr_save()/local_intr_restore()` 保护关键区，保证 current、页表、执行现场一致。

[RELY]

| 依赖 | 位置 | 提供什么 |
|---|---|---|
| `SATP_MODE_SV39` / `RISCV_PGSHIFT` / `PROC_RUNNABLE` | `riscv.h` / `proc.h` | satp 模式位、页大小移位、可运行态 |
| `struct proc_struct` / `struct context` | `proc.h` | PCB；`context` 是 `ra/sp/s0~s11` 14 个 callee-saved |
| `read_csr` / `write_csr` / `flush_tlb` | `riscv.h` / `pmm.h` | 读写 CSR、刷 TLB |
| `boot_pgdir_pa` / `enable_paging()` | `pmm.c` | Sv39 satp 的参考写法 |
| `proc_list` / `current` / `idleproc` / `need_resched` / `runs` | `proc.c` | 进程链表与调度状态 |
| `local_intr_save/restore` | `sync.h` | 关中断保护 |
| `list_next()` / `le2proc()` | `list.h` / `proc.h` | 遍历进程链表 |
| `forkret` / `forkrets` / `__trapret` | `proc.c` / `trapentry.S` | 新线程首次运行恢复链路 |

明确禁止：`lsatp()`（Sv32 遗留，表达不了 Sv39）。

[GUARANTEE]

```c
void cpu_idle(void);                              // kern/process/proc.c
void schedule(void);                              // kern/schedule/sched.c
void proc_run(struct proc_struct *proc);          // kern/process/proc.c
void switch_to(struct context *from, struct context *to);  // kern/process/switch.s
```

签名不可改。

[SPECIFICATION]

- **`cpu_idle`**：不返回的死循环；每轮查 `current->need_resched`，需要就 `schedule()`，否则空转；不选线程、不调 `proc_run`/`switch_to`、不改 `current`。
- **`schedule`**：`local_intr_save` → `current->need_resched = 0` → 用 `list_next`+`le2proc` 从 `proc_list` 找 `PROC_RUNNABLE` 的 next（current 是 idleproc 从表头、否则从 current 下一个轮转）→ 找不到 `next = idleproc` → `next != current` 才 `proc_run(next)` → `local_intr_restore`。
- **`proc_run`**：`proc != current` 才切换；`local_intr_save` → `prev = current` → `current = proc` → `pgdir` 变了就 `write_csr(satp, SATP_MODE_SV39<<60 | pgdir>>RISCV_PGSHIFT)` + `flush_tlb()` → `switch_to(&prev->context, &proc->context)` → `local_intr_restore`。
- **`switch_to`**：a0=from、a1=to；`sd ra,0(a0)`…`sd s11,104(a0)` 存 14 个 callee-saved，再 `ld` 从 a1 恢复同名寄存器，`ret`；不碰 caller-saved、CSR、调度状态。

### 生成测试的提示词

[PROMPT]
不另写测试，用课程自带的 `make grade` 验收。先读 `tools/test_lab4.c`（`test_lab4_schedule` 会 `kernel_thread` 造两个线程、各置 `need_resched` 并反复调 `schedule`，最后断言两个线程都跑过）、`tools/inject.py`（把三个测试注入 `pmm_init` 末尾 / `proc_init` 末尾 / `init_main` 开头）、`tools/grade.sh`（只起一次 QEMU，用 `grep -F` 固定串判分）。实现前先把 `test_lab4_schedule` 的断言当成规格，而不是先写代码再猜测试。

[GUARANTEE]
测试只看串口字符串与 `assert` 是否触发。

[SPECIFICATION]
- 通过条件：`make grade` 输出 `Total Score: 30/30`，且 `check Schedule: OK`。
- `check Schedule` 额外要求三行：`this initproc, pid = 1, name = "init"`、`To U: "Hello world!!".`、`To U: "en.., Bye, Bye. :)"`。
- 失败形态 1：切过去就崩 / 跳到 0 地址 → `switch_to` 偏移与 `struct context` 没对齐。
- 失败形态 2：两个测试线程都跑不到 → `schedule` 没从 current 下一个轮转或没兜底 idleproc。
- 失败形态 3：`satp` 写错 → 页表切换后立刻缺页崩。

### 测试覆盖说明

| 覆盖点 | 测试手段 | 实测 |
|---|---|---|
| idle → init 首次切换 | 串口出现 `this initproc, pid = 1, name = "init"` | ✅ |
| `switch_to` 现场迁移 | 三行 `To U` 消息完整打印 | ✅ |
| `schedule` 轮转公平 | `[Test Schedule] PASS`（两个线程都跑） | ✅ |
| `proc_run` 更新 current/切页表 | 整链不崩 | ✅ |
| 最终评分 | `make grade` | ✅ 30/30 |

实测（2026-11-18）：

```text
  -check PMM:                                OK
  -check Process Create:                     OK
  -check Schedule:                           OK
Total Score: 30/30
```
