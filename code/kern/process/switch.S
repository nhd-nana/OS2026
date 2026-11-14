//LAB4 function3 YOUR CODE
// TODO(2412133 吕鹏哲): 上下文切换。
//   入参：a0 = from（旧线程 context），a1 = to（新线程 context）
//   要做：把 ra, sp, s0~s11 共 14 个 callee-saved 寄存器存进 from，
//         再从 to 逐个取回到同名寄存器，最后 ret（此时 ra 已是 to->ra，
//         也就是 forkret，新线程就从那里开始跑）。
//   注意：这里只搬寄存器，不做任何调度决策。
//
// 下面是占位实现：只保证能链接，不会真正切换现场。
    .globl switch_to
switch_to:
    ret