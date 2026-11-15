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
