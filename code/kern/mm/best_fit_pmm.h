#ifndef __KERN_MM_BEST_FIT_PMM_H__
#define __KERN_MM_BEST_FIT_PMM_H__

#include <pmm.h>

/* lab4 骨架的 default_pmm.c 里 include 了本文件却没有提供它。
 * lab4 中 Best-Fit 的实现直接写在 default_pmm.c 内，对外导出的是
 * default_pmm_manager（pmm.c 的 init_pmm_manager 用的就是这个名字），
 * 因此这里只做声明，保持与 lab2 文件名一致、不改 default_pmm.c 的 include。 */
extern const struct pmm_manager default_pmm_manager;

#endif /* ! __KERN_MM_BEST_FIT_PMM_H__ */
