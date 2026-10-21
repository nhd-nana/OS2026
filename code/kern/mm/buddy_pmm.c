#include <pmm.h>
#include <list.h>
#include <buddy_pmm.h>
#include <stdio.h>

/*
 * Buddy System 物理页分配器（lab2）
 *
 * 以物理页为最小单位，块大小为 2^order 页，order ∈ [0, MAX_ORDER]。
 * 每个 order 维护一个空闲双向链表（节点复用 Page->page_link），
 * 分配时从高阶分裂出低阶块，释放时与相邻 buddy 迭代合并。
 *
 * 已分配块信息（起始页、实际 order、请求页数 n）记录在静态数组
 * alloc_records[] 中，不修改 Page 结构体、不依赖 malloc/libc。
 */

// 物理内存上限 128MiB = 2^15 页，MAX_ORDER = 15 足以容纳整块 RAM。
#define MAX_ORDER           15
// 同时存在的已分配块个数上限
#define MAX_ALLOC_RECORDS   1024

/* 已分配块记录 */
struct alloc_record {
    struct Page *base;   // 分配块起始页（NULL 表示空槽）
    int order;           // 实际分配的 order（块大小 2^order 页）
    size_t n;            // 请求的页数
};

/* 每个 order 的空闲链表头，节点复用 Page->page_link */
static list_entry_t free_list[MAX_ORDER + 1];
/* 空闲物理页总数（按页计数） */
static size_t nr_free = 0;
/* 管理范围：空闲页在 pages 数组中的下标区间 [mng_start, mng_end) */
static size_t mng_start = 0;
static size_t mng_end = 0;
/* 已分配块记录表 */
static struct alloc_record alloc_records[MAX_ALLOC_RECORDS];

/* 把 Page 指针转换为 pages 数组下标 */
static size_t page_index(struct Page *p) {
    return (size_t)(p - pages);
}

/*
 * ceil_log2 - 返回最小 order 使 2^order >= n；n==0 时返回 -1。
 * 用 floor(log2(n-1)) + 1 实现，避免 1<<order 移位溢出。
 */
static int ceil_log2(size_t n) {
    if (n == 0) {
        return -1;
    }
    int order = 0;
    size_t v = n - 1;
    while (v > 0) {
        v >>= 1;
        order++;
    }
    return order;
}

/* 在分配记录表中查找起始页为 base 的记录，未找到返回 -1 */
static int find_record(struct Page *base) {
    for (int i = 0; i < MAX_ALLOC_RECORDS; i++) {
        if (alloc_records[i].base == base) {
            return i;
        }
    }
    return -1;
}

/* 返回一个空闲的分配记录槽位，表满返回 -1 */
static int alloc_record_slot(void) {
    for (int i = 0; i < MAX_ALLOC_RECORDS; i++) {
        if (alloc_records[i].base == NULL) {
            return i;
        }
    }
    return -1;
}

/* 判断 p 是否是 order 阶空闲块的块头（即已挂在 free_list[order] 中） */
static bool is_free_head(struct Page *p, int order) {
    list_entry_t *le = &free_list[order];
    while ((le = list_next(le)) != &free_list[order]) {
        if (le2page(le, page_link) == p) {
            return 1;
        }
    }
    return 0;
}

/*
 * buddy_init - 初始化内部状态，可重复调用（重置为空状态），不处理物理内存。
 */
static void buddy_init(void) {
    for (int i = 0; i <= MAX_ORDER; i++) {
        list_init(&free_list[i]);
    }
    nr_free = 0;
    mng_start = (size_t)-1;  // 哨兵：后续由 init_memmap 收紧为真实范围
    mng_end = 0;
    for (int i = 0; i < MAX_ALLOC_RECORDS; i++) {
        alloc_records[i].base = NULL;
        alloc_records[i].order = 0;
        alloc_records[i].n = 0;
    }
}

/*
 * buddy_init_memmap - 把 [base, base+n) 按“最大对齐且不超过剩余页数”的
 * 2^order 块切分并插入对应 free_list[order]；支持多次调用累加。
 */
static void buddy_init_memmap(struct Page *base, size_t n) {
    if (n == 0) {
        return;
    }
    // 清空这些页的 reserved/property 标记与引用计数
    for (struct Page *p = base; p != base + n; p++) {
        p->flags = 0;
        set_page_ref(p, 0);
        p->property = 0;
    }

    // 更新管理范围（多次调用取并集）
    size_t start = page_index(base);
    size_t end = page_index(base + n);
    if (start < mng_start) {
        mng_start = start;
    }
    if (end > mng_end) {
        mng_end = end;
    }

    // 切分：每次取“最大对齐且不超过剩余页数”的 2^order 块
    size_t remaining = n;
    struct Page *cur = base;
    while (remaining > 0) {
        size_t idx = page_index(cur);
        int order = MAX_ORDER;
        for (; order > 0; order--) {
            size_t blksz = (size_t)1 << order;
            if (blksz <= remaining && (idx % blksz) == 0) {
                break;
            }
        }
        // order==0 必满足（1 <= remaining 且 idx % 1 == 0）
        list_add(&free_list[order], &(cur->page_link));
        nr_free += (size_t)1 << order;
        cur += (size_t)1 << order;
        remaining -= (size_t)1 << order;
    }
}

/*
 * buddy_alloc_pages - 分配至少 n 页的连续物理页，返回块起始页。
 * 失败（n==0 / order 超限 / 无足够空闲 / 记录表满）返回 NULL 且不改变计数。
 */
static struct Page *buddy_alloc_pages(size_t n) {
    if (n == 0) {
        return NULL;
    }
    int order = ceil_log2(n);
    if (order > MAX_ORDER) {
        return NULL;
    }

    // 从 order 向上找第一个非空空闲链表
    int cur = order;
    while (cur <= MAX_ORDER && list_empty(&free_list[cur])) {
        cur++;
    }
    if (cur > MAX_ORDER) {
        return NULL;  // 没有足够空闲页，不改变任何状态
    }

    // 预留分配记录槽位
    int slot = alloc_record_slot();
    if (slot < 0) {
        cprintf("buddy_alloc_pages: allocation record table is full\n");
        return NULL;
    }

    // 取出 free_list[cur] 中的第一个块
    struct Page *block = le2page(list_next(&free_list[cur]), page_link);
    list_del(&(block->page_link));

    // 循环分裂：高半做 buddy 放入低一阶链表，保留低半继续分裂
    while (cur > order) {
        cur--;
        struct Page *buddy = block + ((size_t)1 << cur);
        list_add(&free_list[cur], &(buddy->page_link));
    }

    // 记录并扣减空闲页数
    alloc_records[slot].base = block;
    alloc_records[slot].order = order;
    alloc_records[slot].n = n;
    nr_free -= (size_t)1 << order;

    return block;
}

/*
 * buddy_free_pages - 释放以 base 为起始、请求页数为 n 的已分配块。
 * 非法释放（base==NULL / n==0 / 无匹配记录 / n 不一致）打印警告并返回，
 * 不修改空闲链表与计数。
 */
static void buddy_free_pages(struct Page *base, size_t n) {
    if (base == NULL || n == 0) {
        cprintf("buddy_free_pages: invalid argument (base=%p, n=%d)\n",
                base, (int)n);
        return;
    }

    int slot = find_record(base);
    if (slot < 0 || alloc_records[slot].n != n) {
        cprintf("buddy_free_pages: no matching allocation record (base=%p, n=%d)\n",
                base, (int)n);
        return;
    }

    int order = alloc_records[slot].order;
    alloc_records[slot].base = NULL;
    alloc_records[slot].order = 0;
    alloc_records[slot].n = 0;

    nr_free += (size_t)1 << order;

    // 清理该块的页描述符
    for (struct Page *p = base; p != base + ((size_t)1 << order); p++) {
        p->flags = 0;
        set_page_ref(p, 0);
        p->property = 0;
    }

    list_add(&free_list[order], &(base->page_link));

    // 迭代合并
    while (order < MAX_ORDER) {
        size_t idx = page_index(base);
        size_t buddy_idx = idx ^ ((size_t)1 << order);
        // buddy 块必须完整落在管理范围内
        if (buddy_idx < mng_start || buddy_idx + ((size_t)1 << order) > mng_end) {
            break;
        }
        struct Page *buddy = &pages[buddy_idx];
        // buddy 必须是同 order 的空闲块头（即在 free_list[order] 中）
        if (!is_free_head(buddy, order)) {
            break;
        }
        // 从 free_list[order] 移除 base 与 buddy，合并为高一阶块
        list_del(&(base->page_link));
        list_del(&(buddy->page_link));
        base = (buddy < base) ? buddy : base;
        order++;
        list_add(&free_list[order], &(base->page_link));
    }
}

/* buddy_nr_free_pages - 返回当前空闲物理页总数（按页计数） */
static size_t buddy_nr_free_pages(void) {
    return nr_free;
}

/*
 * buddy_check - 只读检查内部不变量，返回失败数（0 表示成功）。
 * 检查项：各 order 空闲链表无环、节点在管理范围、块头按 2^order 页对齐、
 * 无重复块；空闲页总和 == nr_free；分配记录合法且不与空闲块/其它记录重叠。
 */
int buddy_check(void) {
    int fail = 0;
    size_t page_num = npage - nbase;

    // 1. 逐 order 检查空闲链表
    size_t total_free = 0;
    for (int order = 0; order <= MAX_ORDER; order++) {
        size_t blksz = (size_t)1 << order;
        list_entry_t *le = &free_list[order];

        // 先用有界遍历检测是否有环
        size_t steps = 0;
        size_t limit = page_num + 1;
        bool has_cycle = 0;
        while ((le = list_next(le)) != &free_list[order]) {
            if (++steps > limit) {
                has_cycle = 1;
                break;
            }
        }
        if (has_cycle) {
            cprintf("buddy_check: free_list[%d] contains a cycle\n", order);
            fail++;
            continue;
        }

        // 无环：做范围 / 对齐 / 重复检查
        le = &free_list[order];
        while ((le = list_next(le)) != &free_list[order]) {
            struct Page *p = le2page(le, page_link);
            size_t idx = page_index(p);

            if (idx < mng_start || idx + blksz > mng_end) {
                cprintf("buddy_check: free block @%d (order %d) out of managed range\n",
                        (int)idx, order);
                fail++;
            }
            if ((idx % blksz) != 0) {
                cprintf("buddy_check: free block @%d (order %d) misaligned\n",
                        (int)idx, order);
                fail++;
            }
            // 无重复块：该页号在链表中应恰好出现一次
            int cnt = 0;
            list_entry_t *le2 = &free_list[order];
            while ((le2 = list_next(le2)) != &free_list[order]) {
                if (le2page(le2, page_link) == p) {
                    cnt++;
                }
            }
            if (cnt != 1) {
                cprintf("buddy_check: free block @%d (order %d) duplicated\n",
                        (int)idx, order);
                fail++;
            }
            total_free += blksz;
        }
    }

    // 2. 空闲页总和应等于 nr_free
    if (total_free != nr_free) {
        cprintf("buddy_check: total free pages %d != nr_free %d\n",
                (int)total_free, (int)nr_free);
        fail++;
    }

    // 3. 分配记录合法、不与空闲块/其它记录重叠
    for (int i = 0; i < MAX_ALLOC_RECORDS; i++) {
        if (alloc_records[i].base == NULL) {
            continue;
        }
        struct Page *base = alloc_records[i].base;
        int order = alloc_records[i].order;
        size_t blksz = (size_t)1 << order;
        size_t idx = page_index(base);

        if (order < 0 || order > MAX_ORDER) {
            cprintf("buddy_check: record %d has illegal order %d\n", i, order);
            fail++;
            continue;
        }
        if ((idx % blksz) != 0) {
            cprintf("buddy_check: allocated block @%d (order %d) misaligned\n",
                    (int)idx, order);
            fail++;
        }

        // 与空闲块重叠
        for (int o = 0; o <= MAX_ORDER; o++) {
            size_t fsz = (size_t)1 << o;
            list_entry_t *le = &free_list[o];
            while ((le = list_next(le)) != &free_list[o]) {
                struct Page *fp = le2page(le, page_link);
                size_t fidx = page_index(fp);
                if (idx < fidx + fsz && fidx < idx + blksz) {
                    cprintf("buddy_check: alloc @%d (order %d) overlaps free @%d (order %d)\n",
                            (int)idx, order, (int)fidx, o);
                    fail++;
                }
            }
        }

        // 与其它分配记录重叠
        for (int j = i + 1; j < MAX_ALLOC_RECORDS; j++) {
            if (alloc_records[j].base == NULL) {
                continue;
            }
            size_t jidx = page_index(alloc_records[j].base);
            size_t jsize = (size_t)1 << alloc_records[j].order;
            if (idx < jidx + jsize && jidx < idx + blksz) {
                cprintf("buddy_check: alloc @%d overlaps alloc @%d\n",
                        (int)idx, (int)jidx);
                fail++;
            }
        }
    }

    return fail;
}

/*
 * buddy_dump_free_lists - 只读打印每个 order 的空闲块起始页号、块大小与
 * 空闲页总数，供调试。
 */
void buddy_dump_free_lists(void) {
    cprintf("buddy_dump_free_lists: nr_free = %d pages\n", (int)nr_free);
    for (int order = 0; order <= MAX_ORDER; order++) {
        if (list_empty(&free_list[order])) {
            continue;
        }
        cprintf("  order %2d (block = %4d pages):", order, (int)((size_t)1 << order));
        list_entry_t *le = &free_list[order];
        while ((le = list_next(le)) != &free_list[order]) {
            struct Page *p = le2page(le, page_link);
            cprintf(" @%d", (int)page_index(p));
        }
        cprintf("\n");
    }
}

/*
 * buddy_pmm_check - pmm_manager.check 的 void 签名适配包装。
 * 先做只读不变量检查，再做一次“分配→非法释放→释放”往返自测（状态复原），
 * 任何一步失败即 panic，符合仓库 check 约定。
 */
static void buddy_pmm_check(void) {
    int fail = buddy_check();
    if (fail != 0) {
        cprintf("buddy_pmm_check: buddy_check() reported %d failures\n", fail);
        panic("buddy system invariant check failed");
    }

    size_t before = nr_free;

    // 分配不同大小的块，验证对齐与计数
    struct Page *p0 = buddy_alloc_pages(1);   // order 0
    struct Page *p1 = buddy_alloc_pages(3);   // order 2
    struct Page *p2 = buddy_alloc_pages(5);   // order 3
    assert(p0 != NULL && p1 != NULL && p2 != NULL);
    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(page_index(p0) % 1 == 0);
    assert(page_index(p1) % 4 == 0);
    assert(page_index(p2) % 8 == 0);
    assert(nr_free == before - 1 - 4 - 8);

    fail = buddy_check();
    if (fail != 0) {
        cprintf("buddy_pmm_check: after alloc, %d failures\n", fail);
        panic("buddy system check failed after alloc");
    }

    // 非法释放不应改变空闲计数
    size_t free_before = nr_free;
    buddy_free_pages(p1, 1);        // n 与记录不一致
    buddy_free_pages(NULL, 1);      // base 为 NULL
    buddy_free_pages(p1 + 1, 3);    // 不是块起始
    assert(nr_free == free_before);

    // 合法释放，恢复状态
    buddy_free_pages(p1, 3);
    buddy_free_pages(p2, 5);
    buddy_free_pages(p0, 1);
    assert(nr_free == before);

    fail = buddy_check();
    if (fail != 0) {
        cprintf("buddy_pmm_check: after free, %d failures\n", fail);
        panic("buddy system check failed after free");
    }

    // 分配较大块，验证多级分裂与合并往返
    struct Page *big = buddy_alloc_pages(100);  // order 7
    assert(big != NULL);
    assert(page_index(big) % 128 == 0);
    assert(nr_free == before - 128);
    buddy_free_pages(big, 100);
    assert(nr_free == before);

    fail = buddy_check();
    if (fail != 0) {
        cprintf("buddy_pmm_check: final check %d failures\n", fail);
        panic("buddy system final check failed");
    }

    cprintf("buddy_pmm_check() succeeded!\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_pmm_check,
};
