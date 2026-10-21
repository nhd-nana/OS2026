#ifndef __KERN_MM_BUDDY_PMM_H__
#define __KERN_MM_BUDDY_PMM_H__

#include <pmm.h>

extern const struct pmm_manager buddy_pmm_manager;

/* 只读调试钩子：检查 buddy 分配器内部不变量，返回失败数（0 表示成功）。
 * 不修改任何分配状态、不主动 panic。 */
int buddy_check(void);

/* 只读打印每个 order 的空闲块起始页号、页数与空闲页总数，供调试。 */
void buddy_dump_free_lists(void);

#endif /* !__KERN_MM_BUDDY_PMM_H__ */
