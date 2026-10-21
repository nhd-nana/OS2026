#ifndef __KERN_MM_BUDDY_TEST_H__
#define __KERN_MM_BUDDY_TEST_H__

/* Buddy System 黑盒测试入口。返回失败用例数（0 表示全部通过）。
 * 仅当 pmm_manager 为 buddy_pmm_manager 时才真正运行；否则打印 SKIP 并返回 0。 */
int buddy_test(void);

#endif /* !__KERN_MM_BUDDY_TEST_H__ */
