#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

//LAB4 function3 YOUR CODE
// TODO(2412133 吕鹏哲): FIFO 调度
//   1) local_intr_save 保护
//   2) current->need_resched = 0
//   3) 扫 proc_list 找一个 PROC_RUNNABLE 的 next：
//        当前不是 idleproc → 从 current 的下一个开始（Round-Robin）
//        当前是 idleproc   → 从表头开始
//        一个都没有         → next = idleproc 兜底
//   4) next != current 才 proc_run(next)
//   5) local_intr_restore
// 下面是占位实现：什么也不做。
void schedule(void) {
}


