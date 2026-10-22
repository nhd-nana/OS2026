# Lab2 提示词汇总（prompt.md）

> 本文件汇总 Lab2 全过程中使用的提示词，按任务分组，逐条记录：**目的 → 完整 prompt → 结果 → 后续如何调整**。
> 代码生成类任务统一采用课程要求的四段式框架：[PROMPT] / [RELY] / [GUARANTEE] / [SPECIFICATION]。
> 本实验共 4 个代码任务；任务 3、4 各需**两份 prompt**（生成实现 / 生成测试）。

## 使用的 AI 工具

| 成员 | AI 编程工具 | 底层模型 | 备注 |
|------|------------|---------|------|
| 2412799-葛熠 | VS Code + Copilot（Agent 模式） | DeepSeek V4.1 Flash | 改代码、跑 WSL 命令、提交推送 |
| 2412133-吕鹏哲 | VS Code + DeepSeek | DeepSeek V4 Pro | |
| 2412679-钟一成 | Codex（ChatGPT，Agent 模式） | GPT-5 | 辅助代码阅读、命令执行和实验分析 |

---

## 一、任务 1：page_init（物理内存初始化）

### 1.1 最终提示词（四段式）

````markdown
[PROMPT]
**任务**：修改 kern/mm/pmm.c，实现 page_init()，完成物理内存探测与页描述符初始化；
并把 init_pmm_manager() 的分配器切换为 best_fit_pmm_manager。
**操作要求**：必须在项目中修改实际文件；保留文件内既有代码与注释，
在 `//lab2 function1 <学号>` 标记处补全，并把 `YOUR CODE` 替换为学号。
**输出要求**：直接改文件，给出关键代码与验证方式。

[RELY]
// 宏与常量（kern/mm/memlayout.h、mmu.h）
#define PHYSICAL_MEMORY_OFFSET   0xFFFFFFFF40000000   // 虚实偏移
#define PHYSICAL_MEMORY_END      0x88000000           // 可管理物理内存上界
#define PGSIZE                   4096
#define PPN(la)                  (((uintptr_t)(la)) >> PGSHIFT)
#define ROUNDUP(a, n)            ...
// 全局变量（kern/mm/pmm.c）
struct Page *pages;  size_t npage;  uint64_t va_pa_offset;
const size_t nbase = DRAM_BASE / PGSIZE;              // 0x80000
// 可用函数
get_memory_base(), get_memory_size()   // kern/driver/dtb.c，dtb_init() 已调用
init_memmap(struct Page *base, size_t n), pa2page(uintptr_t pa), panic(), cprintf()
extern char end[];                     // 链接符号，虚拟地址
// 页面标志
SetPageReserved(page) / PageReserved(page) / SetPageProperty(page)

[GUARANTEE]
必须实现的接口：
```c
static void page_init(void);
```
其中必须完成：设置 va_pa_offset；由 DTB 得到 [mem_base, mem_end)；
计算 npage；在 end 之后页对齐处放置 pages 数组；把内核与描述符数组占用的页标记为 Reserved；
对剩余完整空闲页调用 init_memmap()。允许新增局部变量与 static 辅助函数。

[SPECIFICATION]
## page_init
**Pre-Condition**：pmm_init() 已调用 init_pmm_manager()，pmm_manager 非空；dtb_init() 已执行。
**Post-Condition**：va_pa_offset == PHYSICAL_MEMORY_OFFSET；npage == PPN(mem_base + mem_size) 且 > nbase；
pages 非空、页对齐、位于 ROUNDUP(end, PGSIZE) 之后；空闲页总数在 (0, npage - nbase] 内；
内核尾部页与描述符数组所在页均为 PageReserved。
**Case 1（DTB 无效）**：mem_base 或 mem_size 为 0 → panic() 终止，不继续初始化。
**Case 2（内存上界超出可管理范围）**：mem_end > PHYSICAL_MEMORY_END → 截断到 PHYSICAL_MEMORY_END。
**Case 3（无非空完整页）**：描述符数组末尾对齐后已 ≥ mem_end → 打印提示并直接返回，**不得调用 init_memmap()**。
**Case 4（正常）**：从描述符数组之后到 mem_end 的完整页全部注册为空闲区。
**Requirements**：不得把 OpenSBI（0x80000000–0x80200000）、内核镜像、描述符数组任一页放入空闲区；
区分虚拟地址、物理地址与页号；可重复构建运行（make clean && make qemu）。
````

### 1.2 迭代记录

**第 1 次尝试**

- 目的：先得到能编译的骨架版本，确认 DTB 与 `pages` 指针可用。
- prompt 摘要：只给了 [PROMPT] 与 [RELY]（宏与全局变量），[SPECIFICATION] 仅一句"完成物理内存初始化"。
- 结果：能编译、能打印内存范围，但 `make grade` 三项失败：
  - `npage` 用 `PPN(mem_size)`，把大小当地址 → `FAIL (npage=0x8000 expect=0x88000)`
  - `pages` 未页对齐 → `FAIL (pages not page-aligned)`
  - 空闲区从 `mem_base` 起注册 → OpenSBI 与内核镜像被当成空闲页
- 调整：把 `test_page_init()` 的 8 条检查逐条改写成 Post-Condition 与 Case，
  并在 [RELY] 中给出 `PHYSICAL_MEMORY_OFFSET`、`PHYSICAL_MEMORY_END`、`nbase` 的具体数值。

**第 2 次尝试（最终版，即 1.1）**

- 结果：`make` 通过；`make qemu` 输出
  `phys memory: [0x0000000080000000, 0x0000000088000000)`、`npage: 0x88000`、
  `free pages: 0x7cb9`、`check_alloc_page() succeeded!`；`make grade` 的
  `[Test 1] Page Init: PASS`。
- 关键：把"先全标 Reserved、再从描述符数组末尾解封"这一策略写进 [SPECIFICATION]，
  比让 AI 自己决定"哪些页要排除"可靠得多。

---

## 二、任务 2：Best-Fit 分配器

### 2.1 最终提示词

````markdown
[PROMPT]
**任务**：在 kern/mm/best_fit_pmm.c 中实现完整的 Best-Fit 物理内存管理模块，并注册
best_fit_pmm_manager；在 kern/mm/pmm.c 的 init_pmm_manager() 中切换为它。
**操作要求**：只能修改 `//lab2 function2-2 <学号>` 标记处以及文件末尾（用于追加注册结构）；
**不得修改 basic_check() 与 best_fit_check()**。
**输出要求**：直接改文件；说明分配与合并策略；给出验证命令。

[RELY]
// 测试函数（只读，必须满足其状态约定）
list_entry_t free_list_store = free_list;      // 直接引用 free_list
unsigned int nr_free_store = nr_free;          // 直接引用 nr_free
assert(PageProperty(p));                        // 空闲块首必须置 PG_property
assert(!PageProperty(p0));                      // 已分配块首必须清 PG_property
// 框架（kern/mm/pmm.h, libs/list.h, memlayout.h）
struct pmm_manager { name, init, init_memmap, alloc_pages, free_pages, nr_free_pages, check };
free_area_t { list_entry_t free_list; unsigned int nr_free; };
list_init/list_add/list_add_before/list_del/list_next/list_prev/list_empty/le2page
SetPageProperty/ClearPageProperty/PageProperty/PageReserved/set_page_ref
// 参考实现（只读）：kern/mm/default_pmm.c 的 First-Fit

[GUARANTEE]
必须实现（签名与 struct pmm_manager 成员一致）：
```c
static void best_fit_init(void);
static void best_fit_init_memmap(struct Page *base, size_t n);
static struct Page *best_fit_alloc_pages(size_t n);
static void best_fit_free_pages(struct Page *base, size_t n);
static size_t best_fit_nr_free_pages(void);
const struct pmm_manager best_fit_pmm_manager;
```
允许新增 static 辅助函数（如链表插入、合并）；内部状态建议
`static free_area_t free_area;` + `#define free_list (free_area.free_list)` + `#define nr_free (free_area.nr_free)`。

[SPECIFICATION]
## best_fit_alloc_pages(n)
**Pre-Condition**：n > 0。
**Post-Condition**：返回满足 `property >= n` 的**最小**空闲块首地址；该块从链表摘除；
若原块大于 n，则把 `base + n` 起的剩余部分作为新空闲块（`property = 原值 - n`、`SetPageProperty`）保留；
`nr_free` 减去 n；返回块 `ClearPageProperty`。
**Case 1（n > nr_free）**：返回 NULL，链表不变。
**Case 2（无满足块）**：返回 NULL，链表不变。
**Case 3（恰好相等）**：整块摘除，不产生剩余块。
**Requirements**：不得返回 `PageReserved` 的页；返回块的 `ref` 置 0。

## best_fit_free_pages(base, n)
**Pre-Condition**：`base` 是此前由 `alloc_pages` 返回的块首；这些页 `!PageReserved && !PageProperty`。
**Post-Condition**：这 n 页成为一个空闲块（`base->property = n`、`SetPageProperty`），按地址升序插入链表；
与前一块相邻（`prev + prev->property == base`）则合并到前块；再与后一块相邻（`base + property == next`）则合并；
`nr_free` 增加 n。
**Requirements**：合并后只保留一个头页带 `PG_property`，被吞并块的首页必须 `ClearPageProperty`。

## best_fit_init_memmap(base, n)
**Pre-Condition**：这 n 页均为 `PageReserved`（由 page_init 标记）。
**Post-Condition**：逐页清 flags/property/ref 后，`base` 成为长度为 n 的空闲块，按地址升序插入链表，`nr_free += n`。
````

### 2.2 迭代记录

**第 1 次尝试**

- prompt 摘要：以 `default_pmm.c` 为模板，要求"把 First-Fit 改成选择最小满足块"。
- 结果：编译失败 4 处：
  ```
  kern/mm/best_fit_pmm.c:24: error: 'free_list' undeclared
  kern/mm/best_fit_pmm.c:28: error: 'nr_free' undeclared
  kern/mm/best_fit_pmm.c:64: error: 'free_list' undeclared
  kern/mm/best_fit_pmm.c:95: error: 'nr_free' undeclared
  ```
- 原因：测试函数直接引用 `free_list`/`nr_free`，而生成版本用了 `free_area.free_list` 形式，
  没有把内部状态以宏别名暴露成同名标识符。
- 调整：在 [RELY] 里把这两个标识符原样列出，[GUARANTEE] 写明"内部状态命名必须与测试一致"。

**第 2 次尝试**

- 结果：编译通过，运行 panic：
  ```
  assertion failed: !PageProperty(p0)
  ```
- 原因：分配时未对返回块 `ClearPageProperty`；切尾时把剩余块插到了链表头，
  破坏"链表按地址升序"的不变式，后续合并读到错误前驱。
- 调整：在 [SPECIFICATION] 的 Post-Condition 中写出三条不变式：
  返回块必须 `ClearPageProperty`、剩余块插在原位置之后、链表保持地址升序。

**第 3 次尝试**

- 结果：`make qemu` 通过，但 `make grade`：
  ```
  -check best fit: WRONG
  error: missing "[Test 2] Best-Fit: PASS"
  ```
- 原因：只实现了模块，`init_pmm_manager()` 仍指向 `default_pmm_manager`；
  `test_best_fit()` 断言 `pmm_manager == &best_fit_pmm_manager`。
- 调整：把"切换 `init_pmm_manager()`"写进 [PROMPT] 的任务描述，而不是只写算法要求。

**最终结果**

```
  -check page init:                          OK
  -check best fit:                           OK
Total Score: 20/20
```

---

## 三、任务 3：Buddy System

> **负责人：2412133-吕鹏哲**

### 3.1 生成实现的 prompt

````markdown
[PROMPT]
**任务**：从零实现 Buddy System 物理页分配器，新建 kern/mm/buddy_pmm.c 与 kern/mm/buddy_pmm.h，
注册 buddy_pmm_manager。块大小为 2^order 页（order ∈ [0, MAX_ORDER]）：分配时从高阶块分裂出低阶块，
回收时与相邻 buddy 迭代合并回高阶。
**操作要求**：必须先读仓库真实接口（struct Page / struct pmm_manager / libs/list.h / pmm.h），
不要臆造签名；新增 .c 放在 kern/mm/ 下自动参与编译，无需改 Makefile；不修改 pmm.c 与 basic_check()/best_fit_check()。
**输出要求**：直接改文件；解释 pmm_init() 里 init() 与 init_memmap() 的先后调用关系；
给出验证命令与预期输出。

[RELY]
// struct Page（kern/mm/memlayout.h）
struct Page { int ref; uint64_t flags; unsigned int property; list_entry_t page_link; };
// 页号换算（kern/mm/pmm.h）
static inline ppn_t page2ppn(struct Page *p) { return p - pages + nbase; }  // nbase = DRAM_BASE/PGSIZE = 0x80000 = 2^19
// 关键性质：nbase 是 2 的幂，因此 pages 下标 idx = p - pages 与物理页号同余，
// “块按 2^order 页对齐”等价于“idx % (1<<order) == 0”。
// 链表（libs/list.h）：空闲块复用 Page->page_link 做节点
list_init / list_add(插到 head 之后) / list_del / list_next / list_empty / le2page
// 物理内存 128MiB = 2^15 页 → MAX_ORDER = 15
// 类型注意：defs.h 只有 typedef int bool，没有 #define true/false（-Werror 下会报 undeclared）

[GUARANTEE]
必须实现（与 struct pmm_manager 成员签名一致）：
```c
static void buddy_init(void);
static void buddy_init_memmap(struct Page *base, size_t n);
static struct Page *buddy_alloc_pages(size_t n);
static void buddy_free_pages(struct Page *base, size_t n);
static size_t buddy_nr_free_pages(void);
static void buddy_pmm_check(void);   // check 的 void 签名适配器
int buddy_check(void);               // 只读不变量检查，返回失败数，不得 panic
void buddy_dump_free_lists(void);
const struct pmm_manager buddy_pmm_manager;
```
内部状态：free_list[MAX_ORDER+1] 双向链表 + 静态分配记录数组（base/order/n，记录已分配块），
不得 malloc/libc。允许新增 static 辅助函数（ceil_log2、查找/插入记录、判断空闲块头等）。

[SPECIFICATION]
## buddy_init_memmap(base, n)
**Pre-Condition**：n > 0；这 n 页均为 PageReserved（page_init 已标记）。
**Post-Condition**：逐页清 flags/property/ref；把 [base, base+n) 按“最大对齐且 ≤ 剩余页数”切分
为 2^order 块（要求 idx % (1<<order) == 0）插入 free_list[order]；nr_free += n；记录管理范围 [start, end)。

## buddy_alloc_pages(n)
**Pre-Condition**：n > 0。
**Post-Condition**：order = ceil_log2(n)（满足 2^order >= n 的最小阶）；从 >= order 的第一个非空链表
取一块，逐级把高半作为 buddy 放入低一阶链表、保留低半继续；nr_free -= 2^order；记录 (base, order, n) 后返回块首。
**Case 1（n == 0）**：返回 NULL。
**Case 2（order > MAX_ORDER 或无足够空闲）**：返回 NULL，不改变任何状态。
**Case 3（记录表满）**：打印提示并返回 NULL，不改变任何状态。

## buddy_free_pages(base, n)
**Pre-Condition**：base 是此前 alloc_pages 返回的块首，n 与分配时的请求页数一致。
**Post-Condition**：查记录；base 无记录或 n 不匹配 → 打印警告并返回，**不得修改链表与计数**；
否则清记录、nr_free += 2^order、插入 free_list[order]，随后与 buddy 合并：
buddy_idx = idx ^ (1<<order)；若 buddy_idx 在管理范围内且 buddy 是空闲块头（在 free_list[order] 中），
则把两者摘除、块首取 min(base, buddy)、order++ 后重新插入，重复到不能合并为止。
**Requirements**：base == NULL 或 n == 0 同样只打印并返回；合并循环上限 MAX_ORDER。
````

### 3.2 生成测试的 prompt

````markdown
[PROMPT]
**任务**：为 Buddy System 设计并实现内核态测试，新建 kern/mm/buddy_test.c 与 kern/mm/buddy_test.h，
并在 kern_init() 的 pmm_init() 之后调用 buddy_test()。
**操作要求**：只通过黑盒接口 alloc_pages/free_pages/nr_free_pages 与只读 buddy_check() 验证，
不修改 Buddy 分配逻辑；单个用例失败只 cprintf 打印并计数，绝不 panic；
使用 cprintf 输出 PASS/FAIL；最终返回失败用例数。
**输出要求**：直接改文件；说明每个用例覆盖的核心行为与边界；给出运行命令与预期输出。

[RELY]
// 黑盒接口（kern/mm/pmm.h）
struct Page *alloc_pages(size_t n);  void free_pages(struct Page *base, size_t n);
size_t nr_free_pages(void);
// 只读不变量钩子
int buddy_check(void);               // 返回失败数，0 表示内部不变量全部成立
// 输出
cprintf(...)                         // libs/stdio.h
// 入口门控：buddy_check() 依赖 buddy 初始化后的 free_list；当 pmm_manager != &buddy_pmm_manager
// 时（如提交态 best_fit）free_list 是零初始化，直接调用会越界，故 buddy_test() 需先判断再运行。

[GUARANTEE]
必须实现（函数名与签名固定，缺一不可）：
```c
int buddy_test(void);
static int test_init_capacity(void);
static int test_basic_alloc_free(void);
static int test_non_power_of_two(void);
static int test_split(void);
static int test_merge_orders(void);
static int test_fragmentation_exhaustion(void);
static int test_boundary_invalid(void);
static int test_random_stress(void);
static int check_invariants(const char *case_name);
static unsigned int lcg_next(unsigned int *seed);
```

[SPECIFICATION]
**总约定**：buddy_test() 顺序跑 8 个用例；每个用例记录 before = nr_free_pages()，并在用例前后各调一次
buddy_check()；返回失败用例数（0 表示全过），最后打印 "X/Y passed, Z failed"。
**LCG**：lcg_next 必须实现 `*seed = (*seed) * 1103515245u + 12345u`，禁止依赖 libc/rand。
**随机压力 Case**：用 LCG 交替生成分配/释放与大小，每次操作后满足
`nr_free_pages() + 已分配实际页数 == before`；周期性检查分配区间不重叠并调用 buddy_check()。
**非法操作 Case**：alloc_pages(0)、alloc 超过空闲页数、alloc 超过 MAX_ORDER、free(NULL)、
双重释放、释放非块头页、释放 n 与记录不一致——均须安全返回且计数不变。
````

### 3.3 测试覆盖说明

8 个用例按“基础 → 分裂/合并 → 规模压力 → 非法边界 → 随机”分层，每一层都用
`nr_free_pages()` 的**计数守恒**与 `buddy_check()` 的**只读不变量**双保险验证：

| 用例 | 覆盖的核心行为 / 边界 |
|---|---|
| `test_init_capacity` | 初始化后空闲页数非零、`buddy_check()` 通过（基线） |
| `test_basic_alloc_free` | 1/2/4/8 页分配的**块首按 2^order 对齐**、释放后计数恢复到 before |
| `test_non_power_of_two` | 3/5/7/9 等非 2 幂请求向上取整到 `2^ceil_log2(n)`，块首仍按取整后的阶对齐 |
| `test_split` | 大块分配引发多级分裂后，各已分配区间**两两不重叠**，全部释放计数恢复 |
| `test_merge_orders` | 主动构造 buddy 对（alloc 16 → free → alloc 8×2）验证**伙伴合并**；再混入不同 order 的相邻块验证**不误合并** |
| `test_fragmentation_exhaustion` | 连续分配小块直到失败（打满记录表）→ 确认失败那次不改变计数；再交错分配/释放制造碎片，周期性核验计数守恒 |
| `test_boundary_invalid` | `alloc(0)`、超量/超 MAX_ORDER、`free(NULL)`、双重释放、释放非块头、n 与记录不一致——全部安全返回且计数不变 |
| `test_random_stress` | LCG 随机 2000 轮分配/释放，`O(1)` 增量维护已分配页数做计数守恒，周期抽查不重叠 + `buddy_check()` |

这些测试之所以有效：Buddy 的核心不变量只有两条——**块首按 2^order 对齐**与**伙伴合并/分裂后计数守恒**。
前者由 `buddy_check()` 逐链表检查（范围、对齐、无重复、无环），后者由每个用例的
`before == after` 与随机压力用例的每步增量守恒直接约束；非法释放不改状态则由
`buddy_free_pages` 的“记录不匹配即返回”契约单独覆盖。所有检查都是**黑盒 + 只读**，
不依赖实现细节，因此能真实检验一个“换了实现也要满足同契约”的 Buddy 分配器。

---

## 四、任务 4：SLUB

> **负责人：2412679-钟一成**

### 4.1 生成实现的 prompt

````markdown
请在当前 uCore Lab2 中设计不替换全局 pmm_manager 的简化 SLUB。建立在 alloc_page/free_page 之上，为每种对象大小申请 4KB slab，按 8 字节对齐切分对象，用空闲链表分配释放，并用静态元数据避免 malloc。提供 slub_init、slub_alloc、slub_free、slub_check；处理 size=0、超过一页、耗尽、非法释放和重复释放；不得修改 page_init、Best-Fit、Buddy 或 make grade。代码必须适配 nostdinc 内核环境，并说明地址转换和测试设计。
````

### 4.2 生成测试的 prompt

````markdown
请为简化 SLUB 设计黑盒自测，覆盖零大小和超过一页请求失败、同大小对象地址不同、释放后复用、多个对象释放后再分配、槽位耗尽时申请新 slab，以及非法指针和重复释放。测试只使用公开接口，输出通过 cprintf 观察，并说明每个用例验证的不变量。
````

### 4.3 测试覆盖说明

当前自测覆盖零大小、`SIZE_MAX`、超过一页请求失败，同大小对象地址不重复，释放后复用，超过单个 4KB slab 容量后继续分配，以及非法指针和重复释放不破坏状态。`make qemu` 应输出 `slub_test: allocation, reuse, bounds, exhaustion and invalid-free checks passed`。空 slab 不归还页是本实验的明确简化。

---

## 五、环境与构建类提示词

### 5.1 定位"内核完全不执行"（QEMU 版本差异）

**目的**：`make qemu` 只打印 OpenSBI banner、没有任何内核输出，需要判断是内核代码错还是启动方式/模拟器的问题。

**完整 prompt**：

```text
QEMU 启动后只有 OpenSBI banner，没有内核输出。要求：不改动任何代码，
用最小的调试代价判断"内核从未被执行"还是"执行了但没有输出"，并给出判定依据。
```

**结果**：lab1 阶段看 `Domain0 Next Address` 是否为 `0x80200000`（为 0 说明交接失败）；
lab2 阶段该值正常但仍无输出，改用 GDB 单步：

```gdb
si 12   → pc=0x8020002e  satp=0
si  3   → pc=0x80200038  satp=0x8000000000080205   # csrw satp 已生效
si  1   → pc=0x80200000                            # 立刻复位
```

**结论**：`entry.S` 在物理地址上开启 Sv39，物理 PC 在新页表中无映射；
QEMU 7.0 写 satp 后立即重新翻译 → 取指 page fault → 固件复位。
QEMU 4.1.1 沿用旧行为，可正常执行。

**后续调整**：基线改为本地编译的 QEMU 4.1.1（`--enable-debug`）。

### 5.2 编译 QEMU 4.1.1 时的链接错误

**目的**：源码编译在新工具链下失败，需要定位并修复。

**完整 prompt**：

```text
编译 qemu-4.1.1 到链接阶段报错：
  multiple definition of `qemu_add_machine_init_done_notifier'
  vl.c:2510: first defined here
请判断原因是代码问题还是工具链默认值变化，给出最小的修改方案（不要改业务代码）。
```

**结果**：`stubs/machine-init-done.c` 与 `vl.c` 重复定义；注释掉
`stubs/Makefile.objs` 中该 stub，并删除旧的 `libqemuutil.a` 与 `machine-init-done.o` 后重编通过。
同时 `--extra-cflags="-fcommon"` 规避 GCC 10+ 默认 `-fno-common`。

### 5.3 环境变量不生效

**目的**：`export QEMU=...` 写进 `~/.bashrc` 后，`make` 仍使用系统 QEMU 7。

**完整 prompt**：

```text
我在 ~/.bashrc 末尾写了 export QEMU=$HOME/opt/qemu-4.1.1/bin/qemu-system-riscv64，
但 make 里 echo $(QEMU) 是空的，make grade 也用了错误的模拟器。帮我定位原因。
```

**结果**：`.bashrc` 顶部有 `case $- in *i*) ;; *) return;; esac`，非交互 shell 提前返回，
后面的 export 不执行。**解决方案不是"记得 source"**，而是把默认值写进 Makefile：

```make
ifndef QEMU
QEMU_LOCAL := $(HOME)/opt/qemu-4.1.1/bin/qemu-system-riscv64
ifeq ($(wildcard $(QEMU_LOCAL)),)
QEMU := qemu-system-riscv64
else
QEMU := $(QEMU_LOCAL)
endif
endif
```

这样任何人 clone 下来、任何非交互场景（任务、脚本、CI）行为一致。
