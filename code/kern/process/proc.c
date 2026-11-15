#include <proc.h>
#include <kmalloc.h>
#include <string.h>
#include <sync.h>
#include <pmm.h>
#include <error.h>
#include <sched.h>
#include <elf.h>
#include <vmm.h>
#include <trap.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <riscv.h>

/* ------------- process/thread mechanism design&implementation -------------
(an simplified Linux process/thread mechanism )
introduction:
  ucore implements a simple process/thread mechanism. process contains the independent memory sapce, at least one threads
for execution, the kernel data(for management), processor state (for context switch), files(in lab6), etc. ucore needs to
manage all these details efficiently. In ucore, a thread is just a special kind of process(share process's memory).
------------------------------
process state       :     meaning               -- reason
    PROC_UNINIT     :   uninitialized           -- alloc_proc
    PROC_SLEEPING   :   sleeping                -- try_free_pages, do_wait, do_sleep
    PROC_RUNNABLE   :   runnable(maybe running) -- proc_init, wakeup_proc,
    PROC_ZOMBIE     :   almost dead             -- do_exit

-----------------------------
process state changing:

  alloc_proc                                 RUNNING
      +                                   +--<----<--+
      +                                   + proc_run +
      V                                   +-->---->--+
PROC_UNINIT -- proc_init/wakeup_proc --> PROC_RUNNABLE -- try_free_pages/do_wait/do_sleep --> PROC_SLEEPING --
                                           A      +                                                           +
                                           |      +--- do_exit --> PROC_ZOMBIE                                +
                                           +                                                                  +
                                           -----------------------wakeup_proc----------------------------------
-----------------------------
process relations
parent:           proc->parent  (proc is children)
children:         proc->cptr    (proc is parent)
older sibling:    proc->optr    (proc is younger sibling)
younger sibling:  proc->yptr    (proc is older sibling)
-----------------------------
related syscall for process:
SYS_exit        : process exit,                           -->do_exit
SYS_fork        : create child process, dup mm            -->do_fork-->wakeup_proc
SYS_wait        : wait process                            -->do_wait
SYS_exec        : after fork, process execute a program   -->load a program and refresh the mm
SYS_clone       : create child thread                     -->do_fork-->wakeup_proc
SYS_yield       : process flag itself need resecheduling, -- proc->need_sched=1, then scheduler will rescheule this process
SYS_sleep       : process sleep                           -->do_sleep
SYS_kill        : kill process                            -->do_kill-->proc->flags |= PF_EXITING
                                                                 -->wakeup_proc-->do_wait-->do_exit
SYS_getpid      : get the process's pid

*/

// the process set's list
list_entry_t proc_list;

#define HASH_SHIFT 10
#define HASH_LIST_SIZE (1 << HASH_SHIFT)
#define pid_hashfn(x) (hash32(x, HASH_SHIFT))

// has list for process set based on pid
static list_entry_t hash_list[HASH_LIST_SIZE];

// idle proc
struct proc_struct *idleproc = NULL;
// init proc
struct proc_struct *initproc = NULL;
// current proc
struct proc_struct *current = NULL;

static int nr_process = 0;

void kernel_thread_entry(void);
void forkrets(struct trapframe *tf);
void switch_to(struct context *from, struct context *to);

// set_proc_name - set the name of proc
char *
set_proc_name(struct proc_struct *proc, const char *name)
{
    memset(proc->name, 0, sizeof(proc->name));
    return memcpy(proc->name, name, PROC_NAME_LEN);
}

// get_proc_name - get the name of proc
char *
get_proc_name(struct proc_struct *proc)
{
    static char name[PROC_NAME_LEN + 1];
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->name, PROC_NAME_LEN);
}

// get_pid - alloc a unique pid for process
static int
get_pid(void)
{
    static_assert(MAX_PID > MAX_PROCESS);
    struct proc_struct *proc;
    list_entry_t *list = &proc_list, *le;
    static int next_safe = MAX_PID, last_pid = MAX_PID;
    if (++last_pid >= MAX_PID)
    {
        last_pid = 1;
        goto inside;
    }
    if (last_pid >= next_safe)
    {
    inside:
        next_safe = MAX_PID;
    repeat:
        le = list;
        while ((le = list_next(le)) != list)
        {
            proc = le2proc(le, list_link);
            if (proc->pid == last_pid)
            {
                if (++last_pid >= next_safe)
                {
                    if (last_pid >= MAX_PID)
                    {
                        last_pid = 1;
                    }
                    next_safe = MAX_PID;
                    goto repeat;
                }
            }
            else if (proc->pid > last_pid && next_safe > proc->pid)
            {
                next_safe = proc->pid;
            }
        }
    }
    return last_pid;
}


// forkret -- the first kernel entry point of a new thread/process
// NOTE: the addr of forkret is setted in copy_thread function
//       after switch_to, the current proc will execute here.
static void
forkret(void)
{
    forkrets(current->tf);
}

// hash_proc - add proc into proc hash_list
static void
hash_proc(struct proc_struct *proc)
{
    list_add(hash_list + pid_hashfn(proc->pid), &(proc->hash_link));
}

// find_proc - find proc frome proc hash_list according to pid
struct proc_struct *
find_proc(int pid)
{
    if (0 < pid && pid < MAX_PID)
    {
        list_entry_t *list = hash_list + pid_hashfn(pid), *le = list;
        while ((le = list_next(le)) != list)
        {
            struct proc_struct *proc = le2proc(le, hash_link);
            if (proc->pid == pid)
            {
                return proc;
            }
        }
    }
    return NULL;
}


// setup_kstack - alloc pages with size KSTACKPAGE as process kernel stack
static int
setup_kstack(struct proc_struct *proc)
{
    struct Page *page = alloc_pages(KSTACKPAGE);
    if (page != NULL)
    {
        proc->kstack = (uintptr_t)page2kva(page);
        return 0;
    }
    return -E_NO_MEM;
}

// put_kstack - free the memory space of process kernel stack
static void
put_kstack(struct proc_struct *proc)
{
    free_pages(kva2page((void *)(proc->kstack)), KSTACKPAGE);
}


// do_exit - called by sys_exit
//   1. call exit_mmap & put_pgdir & mm_destroy to free the almost all memory space of process
//   2. set process' state as PROC_ZOMBIE, then call wakeup_proc(parent) to ask parent reclaim itself.
//   3. call scheduler to switch to other process
int do_exit(int error_code)
{
    panic("process exit!!.\n");
}

// init_main - the second kernel thread used to create user_main kernel threads
static int
init_main(void *arg)
{
    cprintf("this initproc, pid = %d, name = \"%s\"\n", current->pid, get_proc_name(current));
    cprintf("To U: \"%s\".\n", (const char *)arg);
    cprintf("To U: \"en.., Bye, Bye. :)\"\n");
    return 0;
}

//LAB4 function2 2412679
/* ===== 功能模块二：进程创建及初始化（负责人 2412679 钟一成）==========
 *   alloc_proc    —— 申请一块 PCB，把所有字段置成"未初始化"的干净状态
 *   copy_mm       —— 内核线程共用内核页表，没有独立地址空间，占位返回 0
 *   copy_thread   —— 把 trapframe 模板拷到新内核栈顶，设好 context.ra/sp
 *   do_fork       —— 按 7 步建进程，前 3 步失败要逐级回滚
 *   kernel_thread —— 构造"第一次怎么跑"的 trapframe 模板，再走 do_fork
 *   proc_init     —— 造 idleproc(pid 0) 与 initproc(pid 1)
 * ================================================================== */

// alloc_proc - alloc a proc_struct and init all fields of proc_struct
static struct proc_struct *
alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc == NULL)
    {
        return NULL;
    }
    proc->state = PROC_UNINIT;          // 未初始化态，等 wakeup_proc 变 RUNNABLE
    proc->pid = -1;                     // pid 在 do_fork 里才真正分配
    proc->runs = 0;                     // 还没运行过
    proc->kstack = 0;                   // 内核栈还没分配
    proc->need_resched = 0;
    proc->parent = NULL;
    proc->mm = NULL;                    // 内核线程没有地址空间
    memset(&(proc->context), 0, sizeof(struct context));  // ra/sp/s0..s11 全 0
    proc->tf = NULL;                    // 还没构造 trapframe
    proc->pgdir = boot_pgdir_pa;        // 暂时复用内核页表
    proc->flags = 0;
    memset(proc->name, 0, sizeof(proc->name));
    list_init(&(proc->list_link));      // 挂链表前先自成一个空节点
    list_init(&(proc->hash_link));
    return proc;
}

// copy_mm - kernel threads share the kernel memory space
// 本实验不实现用户地址空间：mm 保持 NULL，函数形态留给 lab5 扩展
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc)
{
    (void)clone_flags;
    proc->mm = NULL;
    return 0;
}

// copy_thread - 把 trapframe 模板放到新内核栈顶，并设好首次切换的入口
static void
copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf)
{
    // trapframe 放在新栈的最高处：kstack + KSTACKSIZE 再减去一个 trapframe 大小
    proc->tf = (struct trapframe *)(proc->kstack + KSTACKSIZE) - 1;
    *(proc->tf) = *tf;                  // 整个结构体拷贝一份模板

    proc->tf->gpr.a0 = 0;               // a0 = 0：子进程（新线程）的"返回值"
    // esp 为 0 表示内核线程：栈顶就用 trapframe 自身，RESTORE_ALL 之后
    // sp 指向这里，线程往下长栈不会覆盖已经用掉的 trapframe
    proc->tf->gpr.sp = (esp == 0) ? (uintptr_t)proc->tf : esp;

    // switch_to 只恢复 ra/sp/s0..s11，所以首次运行必须瞄准 forkret
    proc->context.ra = (uintptr_t)forkret;
    proc->context.sp = (uintptr_t)proc->tf;
}

// do_fork - 创建新进程/线程，成功返回 pid，失败返回 -E_NO_MEM
int
do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf)
{
    int ret = -E_NO_MEM;
    struct proc_struct *proc;
    bool intr_flag;

    // ① 申请 PCB
    if ((proc = alloc_proc()) == NULL)
    {
        goto fork_out;
    }
    // ② 父子关系（initproc 的 parent 因此是 idleproc）
    proc->parent = current;

    // ③ 分配内核栈
    if (setup_kstack(proc) != 0)
    {
        goto bad_fork_cleanup_proc;
    }
    // ④ 地址空间（本实验占位）
    if (copy_mm(clone_flags, proc) != 0)
    {
        goto bad_fork_cleanup_kstack;
    }
    // ⑤ 构造 trapframe 与 context
    copy_thread(proc, stack, tf);

    // ⑥ 分配 pid、挂进全局链表与 hash 表（要关中断保护共享链表）
    local_intr_save(intr_flag);
    {
        proc->pid = get_pid();
        hash_proc(proc);
        list_add(&proc_list, &(proc->list_link));
        nr_process++;
    }
    local_intr_restore(intr_flag);

    // ⑦ 置为可运行，等待调度
    wakeup_proc(proc);

    ret = proc->pid;

fork_out:
    return ret;

bad_fork_cleanup_kstack:
    put_kstack(proc);
bad_fork_cleanup_proc:
    kfree(proc);
    goto fork_out;
}

// kernel_thread - 用 fn(arg) 作为入口创建一个内核线程
int
kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags)
{
    struct trapframe tf;

    memset(&tf, 0, sizeof(struct trapframe));
    // kernel_thread_entry 会执行 "move a0, s1; jalr s0"，所以：
    tf.gpr.s0 = (uintptr_t)fn;              // s0 = 要执行的函数
    tf.gpr.s1 = (uintptr_t)arg;             // s1 = 它的参数
    tf.epc = (uintptr_t)kernel_thread_entry; // sret 到这里开始跑

    // 新线程从 S 态开始，且返回时要把中断重新打开（SPIE=1）
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;

    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}

void
proc_init(void)
{
    int i, pid;

    list_init(&proc_list);
    for (i = 0; i < HASH_LIST_SIZE; i++)
    {
        list_init(hash_list + i);
    }

    // ---- idleproc：没有它 current 就是 NULL，调度器无从落脚 ----
    if ((idleproc = alloc_proc()) == NULL)
    {
        panic("cannot alloc idleproc.\n");
    }
    idleproc->pid = 0;
    idleproc->state = PROC_RUNNABLE;
    idleproc->kstack = (uintptr_t)bootstack;   // 直接复用开机的 8KB 栈
    idleproc->need_resched = 1;                // 一进 cpu_idle 就触发调度
    set_proc_name(idleproc, "idle");
    nr_process++;

    // 先让 current 指向 idleproc，initproc 的 parent 才是 idleproc
    current = idleproc;

    // ---- initproc：第一个真正的内核线程，入口是 init_main ----
    pid = kernel_thread(init_main, "Hello world!!", 0);
    if (pid <= 0)
    {
        panic("create init_main failed.\n");
    }
    initproc = find_proc(pid);
    set_proc_name(initproc, "init");

    /* 说明：指导书提到末尾可调 proc_run(idleproc)，但此处不调用。
     * current 已经是 idleproc（[Test Process Create] 断言的正是这一点）；
     * 若 proc_run 没有 "proc != current" 守卫，switch_to 到 context 全 0
     * 的自己会 ret 到 0 地址而崩溃。第一次真正的切换留给
     * kern_init -> cpu_idle -> schedule。 */
}

//LAB4 function3 YOUR CODE
void
proc_run(struct proc_struct *proc) {
    /* TODO(2412133 吕鹏哲)：local_intr_save → current=proc、runs++、
     * pgdir 变了就写 satp（SATP_MODE_SV39 | (pgdir >> RISCV_PGSHIFT)）+ flush_tlb()
     * → switch_to(&prev->context, &proc->context) → local_intr_restore */
    current = proc;
}

void
cpu_idle(void) {
    while (1) {
        if (current != NULL && current->need_resched) {
            schedule();
        }
    }
}
