# 操作系统实验报告

## 实验基本信息

| 项目 | 内容 |
|---|---|
| 实验名称 | Lab 4：进程管理 |
| 小组成员 | 2412799-葛熠（队长）、2412133-吕鹏哲、2412679-钟一成 |
| 完成日期 | 2026-11-15 |

### 小组分工

| 成员 | 负责的模块 |
|---|---|
| 2412799-葛熠 | lab2/lab3 代码回填、环境与构建、功能模块一 `page_insert` / `page_remove`、报告统稿 |
| 2412133-吕鹏哲 | 功能模块三 `switch_to` / `schedule` / `proc_run` / `cpu_idle` |
| 2412679-钟一成 | 功能模块二 `alloc_proc` / `proc_init` / `kernel_thread` / `do_fork` / `copy_thread` / `copy_mm` |

## 一、实验目的

## 二、实验环境

## 三、实验整体逻辑分析

### 3.1 本章节的逻辑主线

### 3.2 功能的逐步实现

## 四、实验内容与实现

### 功能模块一：虚拟内存页操作

### 功能模块二：进程创建及初始化

**负责人：** 2412679-钟一成

**一、这个模块要解决什么问题**

Lab3 结束时内核已经能被打断、能恢复，但**只有一个执行流**：`kern_init` 一路跑到 `cpu_idle` 死循环。要让内核"同时拥有多个执行实体并轮流使用 CPU"，第一步是先能**造出**这些实体——一个 PCB（进程控制块）、一块内核栈、一份"第一次从哪里开始跑"的现场（trapframe）。

本模块负责"造"，不负责"切"：切换（`switch_to`/`schedule`）属于功能模块三。两者的接口就是 `struct proc_struct` 里的 `context` 和 `tf` 两个字段。

**二、六个函数的职责**

| 函数 | 一句话 | 关键字段 |
|---|---|---|
| `alloc_proc()` | 用 `kmalloc` 拿一块 PCB，把 14 个字段全部置成"未初始化"的干净值 | `state=PROC_UNINIT`、`pid=-1`、`tf=NULL`、`context` 全 0、`pgdir=boot_pgdir_pa` |
| `copy_mm()` | 内核线程共用内核页表，本实验占位 | `proc->mm = NULL; return 0;` |
| `copy_thread()` | 把 trapframe 模板拷到新内核栈顶，设好首次切换的入口 | `context.ra=forkret`、`context.sp=proc->tf` |
| `do_fork()` | 按 7 步把上面几样拼成进程，失败逐级回滚 | `parent`、`pid`、`hash_proc`、`wakeup_proc` |
| `kernel_thread()` | 构造"入口是 fn(arg)"的 trapframe 模板，交给 `do_fork` | `epc=kernel_thread_entry`、`gpr.s0=fn`、`gpr.s1=arg` |
| `proc_init()` | 造 idleproc（pid 0）与 initproc（pid 1），并把 `current` 指向 idle | `kstack=bootstack`、`need_resched=1` |

**三、核心代码**

```c
static struct proc_struct *
alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc == NULL) return NULL;
    proc->state = PROC_UNINIT;
    proc->pid = -1;
    proc->runs = 0;
    proc->kstack = 0;
    proc->need_resched = 0;
    proc->parent = NULL;
    proc->mm = NULL;
    memset(&(proc->context), 0, sizeof(struct context));
    proc->tf = NULL;
    proc->pgdir = boot_pgdir_pa;
    proc->flags = 0;
    memset(proc->name, 0, sizeof(proc->name));
    list_init(&(proc->list_link));
    list_init(&(proc->hash_link));
    return proc;
}

static void
copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf)
{
    proc->tf = (struct trapframe *)(proc->kstack + KSTACKSIZE) - 1;
    *(proc->tf) = *tf;
    proc->tf->gpr.a0 = 0;
    proc->tf->gpr.sp = (esp == 0) ? (uintptr_t)proc->tf : esp;
    proc->context.ra = (uintptr_t)forkret;
    proc->context.sp = (uintptr_t)proc->tf;
}

int
kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags)
{
    struct trapframe tf;
    memset(&tf, 0, sizeof(struct trapframe));
    tf.gpr.s0 = (uintptr_t)fn;
    tf.gpr.s1 = (uintptr_t)arg;
    tf.epc = (uintptr_t)kernel_thread_entry;
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}
```

`do_fork()` 的 7 步与回滚：

```c
int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf) {
    int ret = -E_NO_MEM;
    struct proc_struct *proc;
    bool intr_flag;

    if ((proc = alloc_proc()) == NULL)            goto fork_out;      // ①
    proc->parent = current;                                            // ②
    if (setup_kstack(proc) != 0)                  goto bad_fork_cleanup_proc;   // ③
    if (copy_mm(clone_flags, proc) != 0)          goto bad_fork_cleanup_kstack; // ④
    copy_thread(proc, stack, tf);                                      // ⑤

    local_intr_save(intr_flag);                                        // ⑥
    {
        proc->pid = get_pid();
        hash_proc(proc);
        list_add(&proc_list, &(proc->list_link));
        nr_process++;
    }
    local_intr_restore(intr_flag);

    wakeup_proc(proc);                                                 // ⑦
    ret = proc->pid;

fork_out:            return ret;
bad_fork_cleanup_kstack: put_kstack(proc);
bad_fork_cleanup_proc:   kfree(proc);  goto fork_out;
}
```

**四、三个"必须对齐"的锚点**

新线程第一次运行，是靠 `RESTORE_ALL` 从**人工构造的 trapframe** 恢复现场，然后 `sret` 跳过去。所以下面三处必须一次对上，错一个就是"切过去就崩"或"跳到 0 地址"：

| 锚点 | 谁设置 | 谁使用 |
|---|---|---|
| `context.ra = forkret` | `copy_thread` | 模块三的 `switch_to` 靠它 `ret` 回来 |
| `tf.epc = kernel_thread_entry` | `kernel_thread` | `RESTORE_ALL` 靠它 `sret` 过去 |
| `tf.gpr.s0/s1 = fn/arg` | `kernel_thread` | 框架的 `kernel_thread_entry` 用它 `jalr` 调函数 |

**五、几个设计细节**

1. **`tf.gpr.sp` 为什么在 `esp == 0` 时取 `proc->tf`**：内核线程没有用户栈，第一次运行时 `sp` 就指向自己的 trapframe 顶部；此时 trapframe 已经被 `RESTORE_ALL` 消费完，往下长栈不会覆盖它。
2. **`tf.status` 为什么要置 `SPP` 和 `SPIE`**：新线程必须从 S 态开始跑（否则 `sret` 会按 `SPP=0` 掉到 U 态），并且返回时要把中断重新打开（`SPIE=1` → `sret` 后 `SIE=1`），否则线程跑起来收不到时钟中断。
3. **`proc_init` 里先 `current = idleproc` 再 `kernel_thread`**：`do_fork` 里 `proc->parent = current`，只有这样 `initproc->parent` 才是 `idleproc`（测试会断言）。
4. **`proc_init` 末尾没有调 `proc_run(idleproc)`**：`current` 已经是 `idleproc`，测试断言的正是这一点；若 `proc_run` 没有 `proc != current` 守卫，`switch_to` 到 `context` 全 0 的自己会 `ret` 到 0 地址而崩溃。第一次真正的切换留给 `kern_init → cpu_idle → schedule`。

### 思考题

**思考题：`get_pte()` 把"查页表项"和"分配页表页"合在一个函数里，这样好吗？有没有必要拆开？**

`pmm.c` 里 `get_pte(pgdir, la, create)` 的骨架是这样的：

```c
pte_t *get_pte(pde_t *pgdir, uintptr_t la, bool create) {
    // 第一级：PDX1 对应的中间页表页不存在时，按 create 决定要不要分配
    if (!(*pdep1 & PTE_V)) {
        if (!create || (page = alloc_page()) == NULL) return NULL;
        set_page_ref(page, 1);
        memset(KADDR(page2pa(page)), 0, PGSIZE);
        *pdep1 = pte_create(page2ppn(page), PTE_U | PTE_V);
    }
    // 第二级：PDX0，同样的逻辑再来一遍
    if (!(*pdep0 & PTE_V)) { ... }
    return &((pte_t *)KADDR(PDE_ADDR(*pdep0)))[PTX(la)];
}
```

**好处：**

1. **调用方简单**。`page_insert` 只需要一句 `get_pte(pgdir, la, 1)`，不用自己写"找不到就分配、分配完还要清零、还要挂 PTE"这一串。
2. **一次遍历完成两件事**。查询和补齐都在同一次走表里完成，不会为了"先查再建"重复走两遍三级页表。
3. **语义内聚**。"缺页表页就补"这件事只在一个地方实现，不会在 `page_insert`、`page_remove`、`copy_mm` 里各写一份。

**坏处：**

1. **职责不单一**。一个函数同时干"查询"和"修改"（还会分配内存、改页表），属于命令查询混合。
2. **返回值有歧义**：返回 `NULL` 既可能是"没有映射且 create==0"，也可能是"内存不足分配失败"。调用方**无法区分**这两种情况，只能都当成失败——这是最实质的缺点。`page_insert` 想返回 `-E_NO_MEM`，只能自己再判断一次。
3. **两段代码几乎一样**（PDX1 和 PDX0），是复制粘贴，换成 Sv48 要再加一段；用循环或递归可以消掉。
4. **容易误用**：只想"看看有没有映射"的地方如果漏传 `create=0`，会静默地分配一个页表页，造成内存泄漏。

**有没有必要拆？**

我的结论是：**教学实现可以接受，工程上应该拆开**。合理的拆法是：

- `walk(pgdir, la)`：纯查询，只返回页表项地址或 `NULL`，**不分配、无副作用**；
- `alloc_pgtable(pgdir, la)`：只负责把缺的中间级页表页补上，返回 `0` 或 `-E_NO_MEM`；
- 组合接口保留 `get_pte(pgdir, la, create)`，内部调上面两个。

代价是"创建路径"要多走一遍表，但换来的是：**错误码能区分"不存在"和"没内存"**、查询路径绝对安全、Sv39/Sv48 的层级差异可以用循环而不是复制粘贴。考虑到本实验只有 Sv39 且没有换页压力，保持现状是可接受的；但如果这个内核要继续长到 lab5（缺页处理、`fork` 时复制地址空间），我倾向于先拆。

## 五、测试与验证

### 5.1 运行结果（`make qemu`）

### 5.2 自动评分（`make grade`）

本模块的验收点是 **Test Process Create**（10 分），注入在 `proc_init()` 末尾，断言：

- `current == idleproc`、`idleproc->pid == 0`、`state == PROC_RUNNABLE`、`kstack == bootstack`、`parent == NULL`、`mm == NULL`、`tf == NULL`、`runs == 0`、`flags == 0`、`context.ra == 0`
- `initproc->pid == 1`、`parent == idleproc`、`mm == NULL`、`context.ra != 0 && context.sp != 0`

实测：

```text
  -check PMM:                                OK
  -check Process Create:                     OK
  -check Schedule:                           WRONG
Total Score: 20/30
```

`check PMM` 是功能模块一（葛熠），`check Process Create` 是本模块，`check Schedule` 属于功能模块三（吕鹏哲，尚未合并）。**20/30 中属于本模块的 10 分已经拿到。**

## 六、实验总结与收获

### 6.1 本实验知识点与 OS 原理的对应

### 6.2 OS 原理中重要但本实验未覆盖的知识点

### 6.3 AI 协作开发的经历

## 七、附录：提交记录
