# Lab1 提示词汇总（prompt.md）

> 本文件汇总 Lab1 全过程中使用的提示词，按实验阶段分组，逐条记录：**目的 → 完整 prompt → 结果 → 后续如何调整**。
> 涉及代码/构建文件修改的任务，统一采用课程要求的**四段式框架**：[PROMPT] / [RELY] / [GUARANTEE] / [SPECIFICATION]。

## 使用的 AI 工具

| 成员 | AI 编程工具 | 底层模型 | 备注 |
|------|------------|---------|------|
| 2412799-葛熠 | VS Code + Copilot（Agent 模式） | DeepSeek V4.1 Flash | 直接在仓库中改文件、在 WSL 执行 make/qemu/gdb、提交推送 |
| 2412133-吕鹏哲 | VS Code + DeepSeek | DeepSeek V4 Pro | |
| 2412679-钟一成 | Codex（ChatGPT，Agent 模式） | GPT-5 | 辅助代码阅读、命令执行和实验分析 |

---

## 一、任务梳理与进度规划

### 1.1 从实验文档提炼任务与计划

**目的**：把零散的实验资料（指导书、材料路径、老师要求、报告模板）整理成"要做什么 + 分几步做"。

**完整 prompt**：

```text
见 D:\Code\Course\操作系统\lab1\实验内容.md，总结需要做什么，规划实验进度
```

**结果**：得到 Lab1 的任务清单（跑通最小内核、练习1、练习2、报告、提示词汇总、交付规范）与 P0–P6 阶段划分。

**后续调整**：发现"仓库是空仓库、没有完整代码"这一疑点后，追加确认——**lab1 的代码由课程材料提供，实验本身不需要写新代码**，仓库只需要装 `code/` 与 `report/`。

---

## 二、环境与构建（对应报告 §四 功能模块）

### 2.1 第一次迭代：按指导书直接跑

**目的**：先复现指导书的结果，确认最小内核能跑起来。

**完整 prompt**：

```text
在 WSL（Ubuntu2204）里按实验指导书跑通 lab1：cd 到 code 目录，执行 make clean && make && make qemu，
确认内核能输出 (THU.CST) os is loading ...。不要改动任何代码，把完整输出贴回来。
```

**结果**：编译、链接、生成 `bin/ucore.img` 全部正常，QEMU 也起来了，但输出停在 OpenSBI banner，**完全没有内核输出**。

**后续调整**：不再让 AI"猜原因改代码"，改为要求它先给出**定位方案**。

### 2.2 第二次迭代：先定位问题在哪一层

**目的**：判断"内核从未被执行"还是"执行了但没输出"，避免在内核代码里做无效排查。

**完整 prompt**：

```text
QEMU 启动后只有 OpenSBI banner，没有内核输出。要求：不改动任何代码，用最小的调试代价判断
"内核从未被执行"还是"执行了但没有输出"，并给出判定依据。
```

**结果**：关键证据出现在 OpenSBI banner 中——

```text
Domain0 Next Address      : 0x0000000000000000
```

下一级跳转地址是 0，说明控制权从未交给 `0x80200000`，问题在**装载方式**，不在内核代码。根因：指导书基于 QEMU 4.1.1 + OpenSBI v0.6，而本机是 QEMU 7.0.0 + OpenSBI v1.0（`fw_dynamic`），只有 QEMU 通过 `-kernel` 传入镜像时才会把 next address 设为 0x80200000。

**后续调整**：在正式提示词的 [RELY] 里补上**环境版本**与**实测证据**，并把成功判据写成可观测的输出。

### 2.3 第三次迭代：正式提示词（四段式）

**目的**：修改 Makefile，使内核在本机环境下被正确装载执行。

**完整 prompt**：

````markdown
[PROMPT]
**任务**：修改 code/Makefile 中 qemu 与 debug 两个 target 的 QEMU 命令行，使内核在本机 QEMU 7.0.0 + OpenSBI v1.0 环境下能够被正确装载并执行；不改动任何内核源码逻辑。
**操作要求**：必须直接修改仓库中的实际文件；保留 Makefile 其余内容与变量定义不变，只调整这两个 target 的启动参数。
**输出要求**：给出修改后的片段与验证命令；说明该参数为什么能解决问题，并给出可观测的成功判据。

[RELY]
- 构建产物：$(UCOREIMG) = bin/ucore.img（由 objcopy --strip-all -O binary 生成的纯二进制镜像）
- 链接脚本 tools/kernel.ld：BASE_ADDRESS = 0x80200000，ENTRY(kern_entry)
- 实测现象证据：原命令
  qemu-system-riscv64 -machine virt -nographic -bios default -device loader,file=bin/ucore.img,addr=0x80200000
  启动后 OpenSBI banner 打印 `Domain0 Next Address : 0x0000000000000000`
- 环境：QEMU 7.0.0，-bios default 即 OpenSBI v1.0（fw_dynamic 固件）

[GUARANTEE]
必须修改的接口（Makefile target）：
```
qemu:   $(UCOREIMG) $(SWAPIMG) $(SFSIMG)
debug:  $(UCOREIMG) $(SWAPIMG) $(SFSIMG)
```
两个 target 必须采用一致的装载方式；允许新增注释说明原因；不删除既有 target，不改变其依赖关系。

[SPECIFICATION]
**Pre-Condition**：make 已成功生成 bin/ucore.img；QEMU/OpenSBI 版本与上述一致。
**Post-Condition**：make qemu 时能观察到内核输出 `(THU.CST) os is loading ...`；make debug 在同样装载语义下额外开启 gdbstub（-s -S）并停在复位处，等待 GDB 连接 1234 端口。
**Case 1（qemu target）**：OpenSBI 应把下一级跳转地址设为 0x80200000，CPU 从该地址取到第一条指令（kern_entry）。
**Case 2（debug target）**：除上述行为外，CPU 必须在复位后立即停止（-S）并开放 gdbstub（-s），且断点 `b *0x80200000` 能在控制权交接时命中。
**Requirements**：不得依赖内核源码改动；不得依赖特定旧版本 OpenSBI；`make clean && make && make qemu` 全流程可重复。
````

**结果**：两个 target 改为 `-kernel $(UCOREIMG)` 后，QEMU 中输出

```text
Boot HART MEDELEG         : 0x0000000000f0b509
(THU.CST) os is loading ...
```

**后续调整**：把"可重复构建"的要求落到实处——发现行尾问题后追加下一条提示词。

### 2.4 行尾（CRLF/LF）导致构建不可复现

**目的**：让仓库在任何人 clone 之后都能稳定 `make`。

**完整 prompt**：

```text
仓库里有的文件是 CRLF、有的是 LF（Makefile 恰好是 LF 才没出问题）。请确认行尾会不会影响 WSL 下的
make 构建，并给出一次性的修复方案；要求提交之后所有人 clone 下来的行为一致。
```

**结果**：新增 `.gitattributes`（`* text=auto eol=lf`），并把工作区文件重新 checkout 为 LF，`git ls-files --eol` 全部为 `i/lf w/lf`。

**后续调整**：无。

---

## 三、练习1：内核入口分析

### 3.1 初稿分析

**目的**：回答练习1 的两个问题（`la sp, bootstacktop`、`tail kern_init` 各做了什么、目的是什么）。

**完整 prompt**：

```text
阅读 kern/init/entry.S（连同 tools/kernel.ld、kern/mm/mmu.h、kern/mm/memlayout.h），
说明 la sp, bootstacktop 和 tail kern_init 各自完成了什么操作、目的是什么。
【要求】所有结论必须有实测证据（objdump 反汇编、nm 符号表）支撑；不确定的地方明确说不确定，不要猜。
```

**结果**：得到两条指令的展开形式、`bootstacktop = 0x80203000`、`KSTACKSIZE = 2 × 4096` 等结论；并额外发现 `tail` 被链接器**松弛**成一条 `j`。

**后续调整**：追加复核要求（见下条），避免"看似合理的解释"直接被写进报告。

### 3.2 复核与交叉验证

**目的**：验证初稿中的每个断言，并顺带检查 `kern_init()` 里的 BSS 清零是否真的生效。

**完整 prompt**：

```text
复核以下结论，逐条给出支持或反驳的证据（命令 + 输出）：
1) la sp, bootstacktop 装载的是 0x80203000，且 bootstack 占 8KB；
2) tail kern_init 不保存返回地址，且被链接器优化成 j；
3) kern_init 开头的 memset(edata, 0, end - edata) 是否真的清了 BSS。
```

**结果**：三条全部得到证据支持；其中第 3 条发现 `edata == end == 0x80203008`，**`.bss` 为空，清零是空操作**——这是报告里的加分细节。

**后续调整**：无。

---

## 四、练习2：GDB 验证启动流程

### 4.1 设计可复现的调试方案

**目的**：让"从加电到 0x80200000"的过程可复现、可截图、可写进报告。

**完整 prompt**：

```text
设计一套可复现的 GDB 调试流程，验证从 RISC-V 加电到内核第一条指令（0x80200000）的全过程。
要求：给出两个终端（QEMU 端 / GDB 端）的完整命令；列出每个观察点的命令与预期输出；
最后回答"硬件加电后最初执行的几条指令位于什么地址、主要完成了哪些功能"。
环境：WSL Ubuntu2204，QEMU 7.0.0 + OpenSBI v1.0，内核用 -kernel 装载，make debug/make gdb 已可用。
```

**结果**：得到并实际执行了完整会话——复位 `pc = 0x1000` 的 MROM 六条指令、`si 6` 进入 `0x80000000`、`b *0x80200000` 命中 `kern_entry` 时 `sp = 0x8003def0`（OpenSBI 的栈）、`si 3` 后 `sp = 0x80203000 = bootstacktop`。

**后续调整**：对其中的一条"经验之谈"提出质疑，见下条。

### 4.2 质疑指导书的一处说法并实测

**目的**：确认 `watch *0x80200000` 是否真能观察到"内核被加载的瞬间"。

**完整 prompt**：

```text
指导书建议用 watch *0x80200000 观察内核被加载进内存的瞬间。请实测验证这个说法在本环境下是否成立；
如果不成立，说明原因，并给出更可靠的验证手段。
```

**结果**：复位时 `x/4i 0x80200000` 就已经能看到 `kern_entry` 的指令——**QEMU 在 CPU 复位之前就完成了镜像装载**，该 watchpoint 不会触发（`-device loader` 同理）。更可靠的手段是 `b *0x80200000` + `info registers pc/sp`。

**后续调整**：把这条"与指导书不同的观察"写进报告，作为实测结论而不是简单照抄。

---

## 五、报告与协作文档

### 5.1 按模板写报告

**目的**：一次性产出符合模板要求的报告正文。

**完整 prompt**：

```text
按 D:\文档\课程\大三上\操作系统\实验\实验报告模板.md 完成 Lab1 报告的三、四、五、六节：
三、整体逻辑分析（逻辑主线 + 逐步实现的顺序与理由）；
四、功能模块（把本实验唯一改动的"环境与构建"按四段式提示词 + 迭代过程写）；
五、测试与验证（编译运行输出、GDB 观察、反汇编取证）；
六、总结（知识点与 OS 原理对照表，要写清含义/关系/差异；另列本实验未覆盖的 OS 原理点；AI 协作经验）。
【要求】每个技术结论都要有可复现的证据（命令 + 输出）；不要写无法验证的表述。
```

**结果**：报告正文全部写实，两个练习的结论均带实测证据。

**后续调整**：无。

### 5.2 建立跨会话真相源

**目的**：上下文被压缩或换会话后，仍能立刻恢复工作状态。

**完整 prompt**：

```text
在 D:\Code\Course\操作系统\lab1\ 下建立两份文档并按恶意代码 Lab1 的约定维护：
实验内容.md —— 用户侧输入（实验要求、材料位置、交付规范），Agent 只读不改；
实验进度.md —— 跨会话真相源：当前状态、环境、进度总表、已完成明细（含证据）、待办、坑与经验、命令速查。
每完成一步立刻更新实验进度.md，不要攒着。
```

**结果**：两份文档建立并在每个阶段后同步更新；后续会话可直接据此续做。

**后续调整**：无。

---

## 六、本实验提示词经验小结

1. **把"环境版本 + 实测证据"写进 [RELY]。** 只写"内核没输出"，AI 会往内核代码方向找；补上 `Next Address : 0x0` 和版本号后，方案立刻落到装载方式上。
2. **成功判据要可观测。** "能跑起来"不可验证，"能看到 `(THU.CST) os is loading ...`"才可以。
3. **先定位，再修改。** 用一条"只定位不改代码"的提示词，代价极小却避免了误改内核。
4. **要求 AI 自证。** 所有结论都要求附 `objdump`/`nm`/GDB 的证据，能挡掉"看起来合理"的错误解释。
5. **允许并鼓励质疑文档。** 明确要求"验证指导书的说法"，才发现了 `watch *0x80200000` 在本环境下不成立这一实测差异。