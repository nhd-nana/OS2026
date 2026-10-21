#include <pmm.h>
#include <buddy_pmm.h>
#include <stdio.h>

/*
 * Buddy System 内核态黑盒测试（lab2）
 *
 * 只通过 pmm_manager 的全局包装接口 alloc_pages/free_pages/nr_free_pages
 * 与只读钩子 buddy_check() 验证分配器行为，不修改 Buddy 分配逻辑本身。
 * 单个用例失败只打印、计数失败数，不 panic；panic 仅用于不可恢复的框架错误。
 */

/* 同时存在的已分配记录上限（覆盖耗尽测试约 8k 个 4 页块） */
#define MAX_TEST_ALLOCS 8192

/* 测试用的已分配记录：起始页、请求页数 n、实际块页数 1<<ceil_log2(n) */
struct test_alloc {
    struct Page *base;
    size_t n;
    size_t actual;
};

static struct test_alloc t_allocs[MAX_TEST_ALLOCS];
static int t_nalloc = 0;

/* ceil_log2 - 与 Buddy 分配器一致的 order 计算（本文件私有副本） */
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

/* lcg_next - 简单线性同余伪随机，不依赖 libc/rand */
static unsigned int lcg_next(unsigned int *seed) {
    *seed = (*seed) * 1103515245u + 12345u;
    return *seed;
}

/* check_invariants - 只读检查：调用 buddy_check()，失败打印用例名并返回 1 */
static int check_invariants(const char *case_name) {
    int f = buddy_check();
    if (f != 0) {
        cprintf("  [%s] buddy_check() reported %d failure(s)\n", case_name, f);
        return 1;
    }
    return 0;
}

/* ---- 已分配记录辅助 ---- */

static void t_clear(void) {
    t_nalloc = 0;
}

static int t_record(struct Page *base, size_t n) {
    if (base == NULL || n == 0) {
        return -1;
    }
    if (t_nalloc >= MAX_TEST_ALLOCS) {
        return -1;
    }
    int order = ceil_log2(n);
    t_allocs[t_nalloc].base = base;
    t_allocs[t_nalloc].n = n;
    t_allocs[t_nalloc].actual = (size_t)1 << order;
    t_nalloc++;
    return 0;
}

static int t_find(struct Page *base) {
    for (int i = 0; i < t_nalloc; i++) {
        if (t_allocs[i].base == base) {
            return i;
        }
    }
    return -1;
}

static void t_remove(int i) {
    t_allocs[i] = t_allocs[t_nalloc - 1];
    t_nalloc--;
}

static void t_free_all(void) {
    while (t_nalloc > 0) {
        struct Page *base = t_allocs[t_nalloc - 1].base;
        size_t n = t_allocs[t_nalloc - 1].n;
        t_nalloc--;
        free_pages(base, n);
    }
}

/* 返回 1 表示所有已分配区间两两不重叠，0 表示存在重叠 */
static int t_no_overlap(void) {
    for (int i = 0; i < t_nalloc; i++) {
        size_t a0 = page2ppn(t_allocs[i].base);
        size_t a1 = a0 + t_allocs[i].actual;
        for (int j = i + 1; j < t_nalloc; j++) {
            size_t b0 = page2ppn(t_allocs[j].base);
            size_t b1 = b0 + t_allocs[j].actual;
            if (a0 < b1 && b0 < a1) {
                return 0;
            }
        }
    }
    return 1;
}

static size_t t_total_actual(void) {
    size_t s = 0;
    for (int i = 0; i < t_nalloc; i++) {
        s += t_allocs[i].actual;
    }
    return s;
}

/* ---- 各测试用例 ---- */

static int test_init_capacity(void) {
    int fail = 0;
    if (buddy_check() != 0) {
        cprintf("  [init_capacity] buddy_check() != 0 at start\n");
        fail++;
    }
    size_t n = nr_free_pages();
    if (n == 0) {
        cprintf("  [init_capacity] nr_free_pages() == 0\n");
        fail++;
    }
    cprintf("  [init_capacity] nr_free_pages = %d\n", (int)n);
    return fail;
}

static int test_basic_alloc_free(void) {
    int fail = 0;
    size_t before = nr_free_pages();
    int sizes[4] = {1, 2, 4, 8};
    for (int i = 0; i < 4; i++) {
        size_t n = (size_t)sizes[i];
        struct Page *p = alloc_pages(n);
        if (p == NULL) {
            cprintf("  [basic] alloc_pages(%d) returned NULL\n", (int)n);
            fail++;
            continue;
        }
        if (page2ppn(p) % n != 0) {
            cprintf("  [basic] alloc_pages(%d) start ppn %d misaligned to %d\n",
                    (int)n, (int)page2ppn(p), (int)n);
            fail++;
        }
        t_record(p, n);
    }
    while (t_nalloc > 0) {
        struct Page *base = t_allocs[t_nalloc - 1].base;
        size_t n = t_allocs[t_nalloc - 1].n;
        t_nalloc--;
        free_pages(base, n);
        fail += check_invariants("basic (after free)");
    }
    if (nr_free_pages() != before) {
        cprintf("  [basic] FAIL: count not restored (%d -> %d)\n",
                (int)before, (int)nr_free_pages());
        fail++;
    }
    return fail;
}

static int test_non_power_of_two(void) {
    int fail = 0;
    size_t before = nr_free_pages();
    size_t reqs[4] = {3, 5, 7, 9};
    for (int i = 0; i < 4; i++) {
        size_t n = reqs[i];
        size_t actual = (size_t)1 << ceil_log2(n);
        struct Page *p = alloc_pages(n);
        if (p == NULL) {
            cprintf("  [npot] alloc_pages(%d) returned NULL\n", (int)n);
            fail++;
            continue;
        }
        if (page2ppn(p) % actual != 0) {
            cprintf("  [npot] alloc_pages(%d) start ppn %d misaligned to %d\n",
                    (int)n, (int)page2ppn(p), (int)actual);
            fail++;
        }
        t_record(p, n);
    }
    t_free_all();
    if (nr_free_pages() != before) {
        cprintf("  [npot] FAIL: count not restored (%d -> %d)\n",
                (int)before, (int)nr_free_pages());
        fail++;
    }
    fail += check_invariants("npot");
    return fail;
}

static int test_split(void) {
    int fail = 0;
    size_t before = nr_free_pages();
    size_t sizes[5] = {32, 1, 2, 4, 8};
    for (int i = 0; i < 5; i++) {
        size_t n = sizes[i];
        size_t actual = (size_t)1 << ceil_log2(n);
        struct Page *p = alloc_pages(n);
        if (p == NULL) {
            cprintf("  [split] alloc_pages(%d) returned NULL\n", (int)n);
            fail++;
            continue;
        }
        if (page2ppn(p) % actual != 0) {
            cprintf("  [split] alloc_pages(%d) start ppn %d misaligned to %d\n",
                    (int)n, (int)page2ppn(p), (int)actual);
            fail++;
        }
        t_record(p, n);
    }
    if (!t_no_overlap()) {
        cprintf("  [split] FAIL: allocated blocks overlap\n");
        fail++;
    }
    t_free_all();
    if (nr_free_pages() != before) {
        cprintf("  [split] FAIL: count not restored (%d -> %d)\n",
                (int)before, (int)nr_free_pages());
        fail++;
    }
    fail += check_invariants("split");
    return fail;
}

static int test_merge_orders(void) {
    int fail = 0;
    size_t before = nr_free_pages();
    int k = 3;
    size_t half = (size_t)1 << k;          // 8 页
    size_t full = (size_t)1 << (k + 1);    // 16 页

    // 构造 buddy 对：分配 order4 块 -> 释放 -> 再分配两个 order3 块
    struct Page *big = alloc_pages(full);
    if (big == NULL) {
        cprintf("  [merge] SKIP: cannot allocate %d-page block\n", (int)full);
        t_free_all();
        if (nr_free_pages() != before) {
            cprintf("  [merge] FAIL: count not restored\n");
            fail++;
        }
        return fail;
    }
    free_pages(big, full);

    struct Page *a = alloc_pages(half);
    struct Page *b = alloc_pages(half);
    if (a == NULL || b == NULL || a == b) {
        if (a != NULL) {
            free_pages(a, half);
        }
        if (b != NULL && b != a) {
            free_pages(b, half);
        }
        cprintf("  [merge] FAIL: cannot allocate two %d-page halves\n", (int)half);
        fail++;
        if (nr_free_pages() != before) {
            cprintf("  [merge] FAIL: count not restored\n");
            fail++;
        }
        return fail;
    }

    size_t ia = page2ppn(a), ib = page2ppn(b);
    int buddy_pair = ((ia ^ ib) == half) && (ia % half == 0) && (ib % half == 0);

    if (buddy_pair) {
        cprintf("  [merge] buddy pair: ppn %d <-> %d\n", (int)ia, (int)ib);
        free_pages(a, half);
        fail += check_invariants("merge (free a)");
        free_pages(b, half);
        fail += check_invariants("merge (free b)");
        // 两块已释放应合并为 >= order4，再分配 full 页应成功
        struct Page *c = alloc_pages(full);
        if (c == NULL) {
            cprintf("  [merge] FAIL: merged %d-page alloc failed\n", (int)full);
            fail++;
        } else {
            if (page2ppn(c) % full != 0) {
                cprintf("  [merge] FAIL: merged block ppn %d misaligned to %d\n",
                        (int)page2ppn(c), (int)full);
                fail++;
            }
            free_pages(c, full);
        }
    } else {
        cprintf("  [merge] SKIP: halves not buddy (ia=%d ib=%d xor=%d)\n",
                (int)ia, (int)ib, (int)(ia ^ ib));
        free_pages(a, half);
        free_pages(b, half);
    }

    // 不同 order 的相邻空闲块：释放后不应错误合并
    struct Page *p3 = alloc_pages(3);   // order 2, actual 4
    struct Page *p4 = alloc_pages(5);   // order 3, actual 8
    if (p3 != NULL && p4 != NULL) {
        free_pages(p4, 5);
        free_pages(p3, 3);
        fail += check_invariants("merge (mixed orders)");
    } else {
        if (p3 != NULL) {
            free_pages(p3, 3);
        }
        if (p4 != NULL) {
            free_pages(p4, 5);
        }
    }

    fail += check_invariants("merge (final)");
    if (nr_free_pages() != before) {
        cprintf("  [merge] FAIL: count not restored (%d -> %d)\n",
                (int)before, (int)nr_free_pages());
        fail++;
    }
    return fail;
}

static int test_fragmentation_exhaustion(void) {
    int fail = 0;
    size_t before = nr_free_pages();
    size_t unit = 4;  // 小块：4 页

    // 1) 耗尽：循环分配 4 页块直到 NULL
    int count = 0;
    for (;;) {
        struct Page *p = alloc_pages(unit);
        if (p == NULL) {
            break;
        }
        if (t_record(p, unit) != 0) {
            free_pages(p, unit);
            cprintf("  [frag] FAIL: record table full\n");
            fail++;
            break;
        }
        count++;
    }
    cprintf("  [frag] exhausted after %d x %d-page allocations\n", count, (int)unit);
    if (count == 0) {
        cprintf("  [frag] FAIL: could not allocate any block\n");
        fail++;
    }
    // 确认失败的那次分配不改变计数
    {
        size_t f = nr_free_pages();
        struct Page *pe = alloc_pages(unit);
        if (pe != NULL) {
            free_pages(pe, unit);
        } else if (nr_free_pages() != f) {
            cprintf("  [frag] FAIL: failed alloc changed count\n");
            fail++;
        }
    }

    t_free_all();
    if (nr_free_pages() != before) {
        cprintf("  [frag] FAIL: count not restored after exhaustion (%d -> %d)\n",
                (int)before, (int)nr_free_pages());
        fail++;
    }
    fail += check_invariants("frag (exhaustion freed)");

    // 2) 交错分配/释放制造碎片
    for (int round = 0; round < 1000; round++) {
        struct Page *p = alloc_pages(1 + (size_t)(round % 8));
        if (p != NULL) {
            t_record(p, 1 + (size_t)(round % 8));
        }
        if (t_nalloc > 0 && (round % 3) == 0) {
            int idx = round % t_nalloc;
            struct Page *base = t_allocs[idx].base;
            size_t n = t_allocs[idx].n;
            free_pages(base, n);
            t_remove(idx);
        }
        if (round % 100 == 0) {
            size_t total = t_total_actual();
            if (nr_free_pages() + total != before) {
                cprintf("  [frag] FAIL: count not conserved at round %d (free %d + alloc %d != %d)\n",
                        round, (int)nr_free_pages(), (int)total, (int)before);
                fail++;
                break;
            }
        }
    }
    t_free_all();
    if (nr_free_pages() != before) {
        cprintf("  [frag] FAIL: count not restored after interleave\n");
        fail++;
    }
    fail += check_invariants("frag (final)");
    return fail;
}

static int test_boundary_invalid(void) {
    int fail = 0;
    size_t before = nr_free_pages();

    // alloc_pages(0) == NULL
    if (alloc_pages(0) != NULL) {
        cprintf("  [boundary] alloc_pages(0) != NULL\n");
        fail++;
    }

    // 超过当前空闲页数 -> NULL 且计数不变
    size_t f1 = nr_free_pages();
    struct Page *big = alloc_pages(f1 + 1);
    if (big != NULL) {
        cprintf("  [boundary] alloc_pages(nr_free+1) != NULL\n");
        fail++;
        free_pages(big, f1 + 1);
    }
    if (nr_free_pages() != f1) {
        cprintf("  [boundary] FAIL: count changed after over-available alloc\n");
        fail++;
    }

    // 超过 MAX_ORDER -> NULL（1<<20 页，order 20 > 15）
    size_t f2 = nr_free_pages();
    struct Page *huge = alloc_pages((size_t)1 << 20);
    if (huge != NULL) {
        cprintf("  [boundary] alloc_pages(1<<20) != NULL\n");
        fail++;
        free_pages(huge, (size_t)1 << 20);
    }
    if (nr_free_pages() != f2) {
        cprintf("  [boundary] FAIL: count changed after over-order alloc\n");
        fail++;
    }

    // free_pages(NULL, n) 安全返回
    size_t f3 = nr_free_pages();
    free_pages(NULL, 1);
    free_pages(NULL, 0);
    if (nr_free_pages() != f3) {
        cprintf("  [boundary] FAIL: count changed after free NULL\n");
        fail++;
    }
    fail += check_invariants("boundary (free NULL)");

    // 双重释放 + 释放非块头页
    struct Page *p = alloc_pages(4);
    if (p == NULL) {
        cprintf("  [boundary] FAIL: alloc_pages(4) NULL\n");
        fail++;
    } else {
        free_pages(p, 4);
        size_t f4 = nr_free_pages();
        free_pages(p, 4);       // 双重释放 -> 安全
        if (nr_free_pages() != f4) {
            cprintf("  [boundary] FAIL: double free changed count\n");
            fail++;
        }
        free_pages(p + 1, 1);   // 非块头 -> 安全
        if (nr_free_pages() != f4) {
            cprintf("  [boundary] FAIL: free non-head changed count\n");
            fail++;
        }
        fail += check_invariants("boundary (double free)");
    }

    // 不一致 n 释放：不释放该块；再用正确 n 释放成功
    struct Page *q = alloc_pages(5);   // n=5, actual=8
    if (q == NULL) {
        cprintf("  [boundary] FAIL: alloc_pages(5) NULL\n");
        fail++;
    } else {
        size_t f5 = nr_free_pages();
        free_pages(q, 3);   // 错误 n -> 安全，不释放
        if (nr_free_pages() != f5) {
            cprintf("  [boundary] FAIL: wrong-n free changed count\n");
            fail++;
        }
        free_pages(q, 5);   // 正确 n -> 释放 actual 8 页
        if (nr_free_pages() != f5 + 8) {
            cprintf("  [boundary] FAIL: correct free did not add 8 pages\n");
            fail++;
        }
        fail += check_invariants("boundary (wrong n)");
    }

    // 若内存允许，分配较大 2 幂块（order 14）
    struct Page *big2 = alloc_pages((size_t)1 << 14);
    if (big2 != NULL) {
        if (page2ppn(big2) % ((size_t)1 << 14) != 0) {
            cprintf("  [boundary] FAIL: large order-14 block misaligned\n");
            fail++;
        }
        free_pages(big2, (size_t)1 << 14);
    } else {
        cprintf("  [boundary] note: large order-14 alloc failed (fragmented)\n");
    }

    if (nr_free_pages() != before) {
        cprintf("  [boundary] FAIL: count not restored (%d -> %d)\n",
                (int)before, (int)nr_free_pages());
        fail++;
    }
    fail += check_invariants("boundary (final)");
    return fail;
}

static int test_random_stress(void) {
    int fail = 0;
    size_t before = nr_free_pages();
    unsigned int seed = 42u;
    const int ITER = 2000;
    const int CAP = 256;
    size_t alloc_sum = 0;   // 当前已分配实际页数总和（增量维护）

    for (int iter = 0; iter < ITER; iter++) {
        unsigned int r = lcg_next(&seed);
        int do_alloc;
        if (t_nalloc == 0) {
            do_alloc = 1;
        } else if (t_nalloc >= CAP) {
            do_alloc = 0;
        } else {
            do_alloc = (int)(r & 1u);
        }

        if (do_alloc) {
            size_t n = 1 + (lcg_next(&seed) % 64u);
            size_t actual = (size_t)1 << ceil_log2(n);
            struct Page *p = alloc_pages(n);
            if (p != NULL) {
                if (page2ppn(p) % actual != 0) {
                    cprintf("  [stress] iter %d: alloc(%d) ppn %d misaligned to %d\n",
                            iter, (int)n, (int)page2ppn(p), (int)actual);
                    fail++;
                }
                t_record(p, n);
                alloc_sum += actual;
            }
        } else {
            int idx = (int)(lcg_next(&seed) % (unsigned int)t_nalloc);
            struct Page *base = t_allocs[idx].base;
            size_t n = t_allocs[idx].n;
            alloc_sum -= t_allocs[idx].actual;
            free_pages(base, n);
            t_remove(idx);
        }

        // 计数守恒（O(1)）
        if (nr_free_pages() + alloc_sum != before) {
            cprintf("  [stress] iter %d: count not conserved (free %d + alloc %d != %d)\n",
                    iter, (int)nr_free_pages(), (int)alloc_sum, (int)before);
            fail++;
            break;
        }
        // 周期性：不重叠 + 只读不变量
        if (iter % 200 == 0) {
            if (!t_no_overlap()) {
                cprintf("  [stress] iter %d: overlap detected\n", iter);
                fail++;
                break;
            }
            fail += check_invariants("stress (periodic)");
        }
    }

    t_free_all();
    if (nr_free_pages() != before) {
        cprintf("  [stress] FAIL: count not restored (%d -> %d)\n",
                (int)before, (int)nr_free_pages());
        fail++;
    }
    fail += check_invariants("stress (final)");
    return fail;
}

/*
 * buddy_test - 总入口。依次运行各用例，记录前后空闲页数与 buddy_check()，
 * 汇总打印 X/Y passed, Z failed，返回失败用例数。
 */
int buddy_test(void) {
    if (pmm_manager != &buddy_pmm_manager) {
        cprintf("buddy_test: active pmm_manager is '%s', not buddy; skip all tests\n",
                pmm_manager ? pmm_manager->name : "none");
        return 0;
    }

    struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"init_capacity", test_init_capacity},
        {"basic_alloc_free", test_basic_alloc_free},
        {"non_power_of_two", test_non_power_of_two},
        {"split", test_split},
        {"merge_orders", test_merge_orders},
        {"fragmentation_exhaustion", test_fragmentation_exhaustion},
        {"boundary_invalid", test_boundary_invalid},
        {"random_stress", test_random_stress},
    };
    int n_cases = (int)(sizeof(cases) / sizeof(cases[0]));
    int passed = 0, failed = 0;

    for (int i = 0; i < n_cases; i++) {
        cprintf("buddy_test: running %s ...\n", cases[i].name);
        size_t before = nr_free_pages();
        int pre_fail = buddy_check();
        int case_fail = cases[i].fn();
        t_free_all();   // 兜底清理（各用例应已自行释放）
        size_t after = nr_free_pages();
        int post_fail = buddy_check();

        int ok = (case_fail == 0) && (before == after) &&
                 (pre_fail == 0) && (post_fail == 0);
        if (ok) {
            passed++;
            cprintf("  [%s] PASS\n", cases[i].name);
        } else {
            failed++;
            cprintf("  [%s] FAIL (case_fail=%d before=%d after=%d pre_check=%d post_check=%d)\n",
                    cases[i].name, case_fail, (int)before, (int)after,
                    pre_fail, post_fail);
        }
    }

    cprintf("buddy_test: %d/%d passed, %d failed\n", passed, n_cases, failed);
    return failed;
}
