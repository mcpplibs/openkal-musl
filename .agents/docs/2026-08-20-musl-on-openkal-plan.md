# 在 openkal 之上重定向 musl:实施方案与验证判据

**状态**:已实施。本节记录结果与方案的偏离;其余各节保留为当初的判断记录,不作追改,
因为一份被结果修改过的方案不再能说明结果是否被预见。

## 0. 结果

| 方案 | 结果 |
| --- | --- |
| 目标系统 Linux 与 macOS | Linux、macOS、Windows 三个系统 |
| 「musl 目前只支持 Linux」 | 成立,且三个系统上的 CI 各自运行同一份源码 |
| 接缝为 `syscall_arch.h` 七个内联函数 | 成立,未增减 |
| musl 自身源码不改 | 四行改动,九个源文件被替换,逐条记于 `musl/PATCHES.md` |
| port 层规模约 2000 行 | 2793 行,增量全部关于目标文件格式与命名约定,不关于内核 |
| openkal 不因 musl 而改变形状 | 不成立:openkal 增加了五个错误值、五个操作与两条命名规则 |

最后一行是本方案唯一的实质性误判,值得单独说明。方案主张 POSIX 在 port 层被重建、
openkal 保持不变。前半句成立;后半句不成立,并且不成立的方式与「为 musl 让步」无关 ——
增加的每一项都是**任何**位于 openkal 之上的 C 库都需要而当时的接口无法表达的:

- `kal_err_not_found` 等五个错误值:一个 C 库必须把 `ENOENT` 与 `EEXIST` 区分开,
  0.4 把两者映射到同一个值;
- `kal_fs_open`:创建、排他、截断、追加是**打开这件事**的条件而非其后的操作,
  分两步做的程序可以在两步之间被停止;
- `kal_fs_truncate`、`kal_fs_file_info`:对已打开文件的操作无法用名字表达,
  因为名字此刻可能已指向别的东西;
- `kal_fs_set_modified`:接口报告一个时间而不能设定它,则复制文件并保留日期、
  展开归档、把文件标记为当前这三类程序都写不出来;
- `KAL_STREAM_PROP_INTERACTIVE`:C 库必须在传输任何字节之前选定缓冲策略;
- `KAL_TASK_PROP_THREAD_LOCAL`:C 库把每上下文状态放在何处,取决于启动的上下文
  是否观察到工具链的线程局部存储,而这一点无法由操作探知;
- `"."` 为保留名:程序持有一个目录却无法就该目录本身发问,因为所有发问的操作都取名字。

也就是说,把一份真实的 C 库放到 openkal 之上,所暴露的是**接口的不完备**而不是
musl 的特殊性。这正是 §7 所说的完备性判据在实践中的形态:判据不是「移植成功」,
而是「移植过程中接口被迫增加了什么,以及每一项是否对所有消费者都成立」。

三条在方案中未被预见、且只能由运行发现的结论:

1. 位于 C 库之下的 openkal 实现**不得借用任何 C 库**。程序自身定义 `write`、`malloc`、
   `open`,实现若调用这些名字,调用解析到程序的定义,而程序的定义又调用实现 ——
   递归无界,且在任何一侧的源码中都不可见。openkal-linux 与 openkal-macos 因此
   改写为直接发出内核调用。
2. 一个 C 库**不能**把每上下文状态放在 `thread_local` 变量中。在三种工具链之一上,
   这样的变量经由一个会分配内存的辅助函数到达,而分配器正是该 C 库提供的。
3. 一个上下文标识为零的实现,对键控于该标识的消费者而言等同于「没有条目」。
   这一条由 macOS 上的一次崩溃发现,并已成为一致性套件的两条观察。

## 0.1 验证

| | |
| --- | --- |
| openkal 一致性套件 | 97 项观察,三个实现各自 97/97 |
| `examples/posix` | 32 项断言,三个系统 |
| `examples/wordcount` | 与系统自带 `wc` 的三个计数逐一相同 |
| `mcpplibs/sbase` | 97 个 suckless 工具,源码未改,50 项与系统自带工具的比对 |

最后一项是方案 §6.2 所要求的对照实验的实际形态。sbase 在 Windows 上没有任何移植,
在 macOS 上也不能按现状构建 —— 三个工具包含 `<sys/sysmacros.h>`,四个使用 `st_mtim`,
两者都是 Linux C 库的东西。这两处障碍都不在内核里,而在 C 库里,因而是一个 C 库能够
消除的那一类;它消除的方式是**存在**,而不是被适配。

**目标**:把 musl 1.2.5 重定向到 openkal 接口之上,使同一份 musl 源码在 Linux 与
macOS 两个操作系统上构建并通过其自身的测试套件。musl 目前只支持 Linux;若它经 openkal
在 macOS 上运行,则该结果证明 openkal 的抽象足以承载一份为单一内核编写的 C 库。

**当前包的定位**:仓库现有的 `openkal-libc` 0.2.0 共 243 行,导出 14 个名字。
它不是 C 库,是一个探针,只做规范刻意置于自身之外的两件事 —— 把全局名字解析到环境
提供的目录上,以及从挂起原语构造同步对象。本方案实施后,该探针的路径解析部分被
musl 的 port 层吸收,包本身按 §8 处理。

本文中标注为「实测」的数据在本机测得,载荷版本随文注明;其余为方案主张。

---

## 1. 架构:POSIX 在何处被重建

openkal 的设计明确排斥 POSIX 形状:规范 §14 记录「core 中没有 `open(path)`」,
理由是 WASIp1 照搬 fd 加路径命名空间,导致每个非 POSIX 宿主都要模拟 preopen 与
`openat`。

因此本方案的第一条决定是:**POSIX 在 musl 的 port 层被重建,openkal 不因 musl 而改变
形状**。

```
        应用(POSIX 源码)
              │  open(2) / pthread_create / printf
        ┌─────┴──────────────────────────────┐
        │  musl 1.2.5                        │
        │  ┌──────────────────────────────┐  │
        │  │ port 层(本方案新增)         │  │  ← POSIX 在这里被重建
        │  │  路径 → preopen 相对名       │  │
        │  │  fd 表 → kal 句柄            │  │
        │  │  clone → kal_task_start      │  │
        │  └──────────────────────────────┘  │
        └─────┬──────────────────────────────┘
              │  openkal C ABI(47 个名字)
        ┌─────┴──────────────────────────────┐
        │  openkal-linux / openkal-macos     │
        └────────────────────────────────────┘
```

该分层的一个直接推论是:**port 层的行数是这次实验的度量**。openkal 若分解正确,
port 层薄;若分解错误,port 层会长出表、注册中心或解析器,而规范 §13.1 的对下判据
正是「计量仅为迁就形状而存在的行数」。

---

## 2. 接缝:实测

### 2.1 musl 到内核的唯一入口

实测(musl 1.2.5):

| 项 | 数量 |
|---|---|
| `.c` 源文件总数 | 1600 |
| 引用 `__syscall` 的 `.c` | 149 |
| 每个体系结构的 `syscall_arch.h` | 18 份,各含 `__syscall0` 至 `__syscall6` 七个内联函数 |
| 直接内联汇编发起系统调用的 `.c` | 4,其中 3 个属于本方案不涉及的体系结构(mipsn32、x32) |

因此重定向的主体是**替换一份头文件中的七个内联函数**,而非改动 1600 个文件。

### 2.2 一个真实程序实际用到的系统调用

实测(x86_64-linux-musl-gcc,`-static`,程序含文件读取、`malloc`、一个线程、
`printf`):`strace -c` 记录 15 个不同的系统调用。

| 系统调用 | 次数 | 到 openkal 的映射 |
|---|---|---|
| `open` / `read` / `close` | 各 1 | `kal_fs_open_file` / `kal_stream_read` / `kal_fs_close_file` |
| `writev` | 1 | `kal_stream_write`,按段循环 |
| `futex` | 3 | `kal_task_wait` / `kal_task_wake`,语义逐字对应 |
| `clone` | 1 | 无对应,见 §3.1 |
| `mmap` / `munmap` / `mprotect` / `brk` | 3/2/1/2 | 无对应,见 §3.2 |
| `arch_prctl` | 1 | 无对应,见 §3.3 |
| `rt_sigprocmask` | 5 | openkal 无信号,置为无操作 |
| `set_tid_address` | 1 | Linux 记账,置为无操作 |
| `ioctl` | 1 | `isatty` 探测,返回 `ENOTTY` |
| `execve` | 1 | 由加载器执行,不经 musl |

**15 个中有 11 个可直接映射或安全地置为无操作,4 个构成真实障碍,且这 4 个属于同一族。**

### 2.3 体系结构相关汇编

实测(x86_64):30 个 `.s` 文件中,26 个存在有实质内容的 C 版本可回退。
四个没有:`setjmp`、`longjmp`、`sigsetjmp`、`syscall_cp`。

此处记录一条方法上的错误,因为它险些写进本方案:第一次测量按「同名 `.c` 是否存在」
判定,得出「30 个全部有 C 版可回退」。实际上 musl 为这四项放置了**零字节的占位
`.c` 文件**,使构建系统不致失败。判据必须是文件内容而非文件存在。

---

## 3. 四个真实障碍

四个障碍全部属于同一族:**地址空间与执行上下文**。openkal 的设计把这一族划归
openarch,而 openarch 尚不存在(设计文档 §10.4、§12)。

### 3.1 `clone`:musl 要求调用方控制线程指针

musl 的 `__clone` 是每个体系结构一份的汇编函数,签名为:

```c
int __clone(int (*fn)(void*), void* stack, int flags,
            void* arg, pid_t* ptid, void* tls, pid_t* ctid);
```

调用方提供栈、提供 TLS 块、并要求被创建的上下文以该 TLS 块启动。
`openkal.task` 提供的是 `kal_task_start(entry, arg, out)` —— 一个完整的执行上下文,
栈与线程指针均由实现分配。二者不可互相表达。

**在 macOS 上不存在任何等价机制**:该系统没有 `clone`,线程只能经 `pthread_create`
创建,而线程指针由系统设置。

有利的一点是 musl 已预期这种移植:`src/thread/clone.c` 是一个返回 `-ENOSYS` 的通用桩,
存在的目的正是让不具备 `clone` 的移植目标能够构建。

**解法**:不重定向 `__clone`,而是**替换 `pthread_create` 的下半部**。
`src/thread/pthread_create.c` 中调用 `__clone` 的那一处改为调用 `kal_task_start`,
线程指针的取得改用 §3.3 的方案。这是对 musl 源码的替换,而非重定向;
是本方案中唯一必须修改 musl 既有 `.c` 的地方。

### 3.2 `mmap` 族:openkal 没有地址空间操作

`openkal.memory` 只有 `kal_alloc` 与 `kal_free`,没有映射、解除映射与保护属性。
musl 用 `mmap` 做三件事:分配器的堆区、线程栈、以及栈的保护页。

| 用途 | 用 `kal_alloc` 能否满足 | 代价 |
|---|---|---|
| 分配器堆区 | 能 —— `mmap(MAP_ANON)` 对 musl 而言即「给我一块对齐的清零内存」 | 无 |
| 线程栈 | 能 | 无 |
| 保护页(`mprotect(PROT_NONE)`) | 不能 | 栈溢出由「静默越界」取代「立即崩溃」 |
| `mremap` | 不能 | musl 的 `realloc` 退化为分配加复制 |

**解法**:port 层实现一个 `mmap` 的子集,`MAP_ANON | MAP_PRIVATE` 转 `kal_alloc`,
其余标志返回 `ENOSYS`。`mprotect` 返回成功而不做任何事,并在文档中记录该退化。

**这一条须提请 review**:让 `mprotect` 声称成功而不生效,违反规范 §2.2 的判别式
——「若一个虚假的实现会使上层静默地产生错误结果,则它是模拟」。栈保护页的缺失
正是会导致静默错误的一类。备选方案是让 `mprotect` 返回 `ENOSYS`,并接受 musl
在无保护页的情况下继续运行(musl 在 `mprotect` 失败时的行为需实测确认)。

### 3.3 线程指针:openkal 明确不认领

实测,musl 取得当前线程的全部接缝是每个体系结构四行:

```c
// arch/x86_64/pthread_arch.h
static inline uintptr_t __get_tp() { uintptr_t tp; __asm__ ("mov %%fs:0,%0" : "=r"(tp)); return tp; }

// arch/aarch64/pthread_arch.h
static inline uintptr_t __get_tp() { uintptr_t tp; __asm__ ("mrs %0,tpidr_el0" : "=r"(tp)); return tp; }
```

38 个 `.c` 文件依赖由此得到的 `__pthread_self()`。

openkal 的设计文档 §10.4 明确写道:`thread_local` 不属于线程契约,属于 openarch
(TLS 寄存器约定)与 BSP。因此 openkal 不提供设置线程指针的手段,这是刻意的。

**解法**:在宿主环境下,线程指针寄存器**已经由宿主的线程机制设置**
—— `kal_task_start` 在两个实现上都建立在系统线程之上。port 层因此以宿主工具链的
`__thread` 变量保存 musl 的 `pthread` 结构指针,`__get_tp()` 改为读取该变量:

```c
// port 层:宿主工具链的 TLS 由宿主线程机制维护,musl 只需一个槽
static __thread uintptr_t okl_tp;
static inline uintptr_t __get_tp(void) { return okl_tp; }
```

**代价与诚实边界**:该方案使 musl 的 TLS 建立在宿主工具链的 TLS 之上,因而
**不适用于裸机**。裸机上的 musl-on-openkal 需要 openarch 提供线程指针约定,
这是本方案不覆盖的部分,且是 §7 判据 3 未达成的根本原因。

### 3.4 `setjmp` 与 `longjmp`:无 C 版可回退

四个仅有汇编实现的名字中,`syscall_cp` 由 port 层重写(它只是可取消版本的
`__syscall`,取消功能在 openkal 上不存在,直接转 `__syscall` 即可),
其余三个是 `setjmp` 族。

musl 的 x86_64 与 aarch64 `setjmp.s` 是 ELF 语法的 GAS 汇编。在 macOS 上须转为
Mach-O 语法(主要差异是符号前缀与段指示),或改用宿主工具链的 `setjmp`。

**解法**:优先复用宿主工具链的 `setjmp`/`longjmp`,因为 `jmp_buf` 的布局是
port 层的内部事务,不进入 openkal 契约。若不可行,则转写三个文件的汇编语法。

---

## 4. macOS 上的额外障碍

musl 从未在 macOS 上构建过。除 §3 之外,以下是仅在 macOS 上出现的问题,须在
第一阶段即验证,因为其中任何一条不成立都会改变整个方案的可行性。

| 问题 | 说明 | 验证方式 |
|---|---|---|
| 目标文件格式 | musl 的构建系统假定 ELF;`.s` 文件是 ELF 语法 | 按 §3.4 处理后,以 `clang -c` 逐文件编译计数 |
| 符号前缀 | Mach-O 的 C 符号带前导下划线 | 已知问题;`check-surface.sh` 已处理该差异 |
| 弱符号与 `.weak` | musl 大量使用弱别名 | Mach-O 支持弱定义,但语法不同 |
| 段名 | `.tdata`/`.tbss` 在 Mach-O 中名称不同 | 若采用 §3.3 的宿主 TLS 方案则不涉及 |
| 与 libSystem 共存 | 最终程序中 musl 与 libSystem 同时存在,两者都定义 `malloc`、`printf` 等 | **这是最大的未知**,见下 |

**符号冲突是 macOS 上的核心风险。** openkal-macos 建立在 libSystem 之上
(`kal_alloc` 转 `malloc`,`kal_stream_write` 转 `write`),因此最终可执行文件中
必然同时存在 libSystem 与 musl,而两者定义同名的 POSIX 符号。

三条候选对策,须在第一阶段选定:

1. **符号重命名**:musl 的全部公开符号加前缀(`musl_malloc` 等),应用经头文件宏
   映射。改动大,但完全消除冲突。
2. **两阶段链接**:将 musl 编译为单一目标文件,用 `ld -r` 合并后将非 openkal 依赖的
   符号局部化。依赖平台链接器能力,Linux 与 macOS 的做法不同。
3. **openkal-macos 不依赖 libSystem**:直接经 `syscall` 或 Mach 陷入。工作量大,
   且 macOS 不保证系统调用号稳定 —— 该系统的稳定接口就是 libSystem。

**倾向对策 2**,理由是它不改变 musl 的公开接口,也不要求 openkal-macos 重写。
但它是本方案中确定性最低的一环,应当在第一阶段以一个最小程序验证,而非等到 musl
全部编译通过之后。

---

## 5. 分阶段计划与任务依赖

```
A. port 层骨架 ──┐
                 ├──► C. 单线程可用 ──► D. 多线程可用 ──► E. libc-test
B. 符号共存验证 ─┘                              │
                                                └──► F. macOS 验证
```

| 阶段 | 内容 | 完成判据 | 依赖 |
|---|---|---|---|
| **A** | `syscall_arch.h` 替换为调用 `__okl_syscall`;实现 `read`/`write`/`writev`/`open`/`close`/`lseek`/`fstat` 与 `exit_group` | 一个只调用 `write(1,...)` 的程序链接 musl 并输出正确内容 | — |
| **B** | 按 §4 选定符号共存对策,以最小程序验证 | 同一可执行文件中 musl 的 `malloc` 与 libSystem 的 `malloc` 各自被正确调用,以 `nm` 与运行结果双向确认 | — |
| **C** | `mmap` 子集、`brk`、路径解析(吸收现有 `okc::resolve`)、`stat` 族、目录枚举 | `printf`、`fopen`/`fread`/`fclose`、`malloc`/`free`、`opendir`/`readdir` 全部可用;`wordcount` 示例改为纯 POSIX 源码后输出与系统 `wc` 一致 | A、B |
| **D** | `pthread_create` 下半部替换为 `kal_task_start`;`__get_tp` 改为宿主 TLS;`futex` 转 `kal_task_wait`/`kal_task_wake` | 四个上下文各自递增 20000 次,总数为 80000 且无丢失;`pthread_mutex`、`pthread_cond`、`pthread_once` 可用 | C |
| **E** | 接入 musl 自己的 libc-test 套件 | 报告通过、失败与跳过的逐项计数,并对每一项失败给出归因:openkal 缺口、port 层缺陷、或该测试依赖 Linux 特有语义 | D |
| **F** | 在 macOS 上重复 A 至 E | 与 Linux 相同的计数表,并逐条对比差异 | E |

阶段 A 与 B 相互独立,可并行。阶段 B 的结论若为「三条对策均不可行」,则整个方案在
macOS 上不成立,应当在此处停止而非继续。**把 B 排在第一批,目的正是让这个停止信号
尽早出现。**

---

## 6. 验证:判据必须是数,不是「跑起来了」

### 6.1 逐阶段的可证伪判据

每个阶段的判据都写成一个可以为假的陈述,理由是本轮 openkal 的经验:
四条流水线在含 ABI 缺陷的已发布包上一直是绿的,原因是断言不观察目标。

| 阶段 | 判据(可证伪形式) |
|---|---|
| A | 程序输出的字节序列与预期逐字节相同,且 `strace` 记录中不出现 `openat` 之外的文件系统调用 |
| B | `nm` 显示 musl 的符号在最终制品中存在且未被 libSystem 的同名符号取代,方式是各自打印一个可区分的标记 |
| C | `wordcount` 的三个计数与系统 `wc` 逐字段相同;若只比较「有输出」则该判据无效 |
| D | 80000 次递增无丢失。该判据必须在**关闭** port 层的互斥实现后失败,以证明它确实在测同步 |
| E | 通过项数、失败项数、跳过项数三者之和等于套件总项数。**跳过项必须逐条列出原因** |
| F | macOS 与 Linux 的三元组差异逐条有归因 |

### 6.2 必须做的对照实验

- **revert 探针**:每一条新增断言,在对应实现被撤销后必须失败。本轮 openkal 的
  argv 断言即以此方式确认,而其上一版套件启动 `/bin/true` 并读取状态,
  无论参数向量完整到达还是被移位一位都得到相同结果。
- **双 libc 对照**:同一份 POSIX 源码分别以宿主工具链和 musl-on-openkal 构建,
  比较输出。差异即缺陷,无差异不构成正确性证明但排除了整类错误。
- **openkal 侧不变**:实施过程中若发现需要修改 openkal 规范,该修改须单独提出并
  论证,不得为迁就 musl 而扩大 openkal。规范 §13.1 记录:对上判据不满足意味着资源
  分解切分有误,应当返回修改分解,而非修改边界以迁就标准库。

### 6.3 本方案将会暴露的诚实边界

以下三条在方案设计阶段即可预见,应当在文档中先行写明,而非等到实测后补记:

1. **不覆盖裸机。** §3.3 的线程指针方案建立在宿主工具链的 TLS 之上。
   裸机上的 musl-on-openkal 需要 openarch,本方案不涉及。
2. **不构成「openkal 足以取代内核」的证明。** openkal-linux 与 openkal-macos 都建立
   在宿主 libc 之上,因此 musl-on-openkal 的最终形态是 musl → openkal → 宿主 libc,
   其中有两份 C 库。它证明的是 musl 的源码可以重定向到 openkal 的接口,
   而非 openkal 在没有 libc 的环境下足够。后者需要裸机实现,属于 §7 判据 3。
3. **信号不被支持。** openkal 没有异步交付机制,port 层将 `rt_sigprocmask` 族置为
   无操作。依赖信号的 libc-test 项目会失败,且这些失败应当归类为「openkal 缺口」
   而非「port 层缺陷」。

---

## 7. 与完备性判据的关系

`.agents/docs` 中 openkal 的完备性计划列出五条判据,本方案影响其中三条。

| 判据 | 当前 | 本方案完成后 |
|---|---|---|
| 1. C 库重定向到 openkal 并通过其自身测试套件 | 部分达成:243 行探针,musl 未重定向 | 达成或给出逐项失败归因 |
| 2. 其上的编译器工具链 | 未达成,未尝试 | 具备前提;仍未达成 |
| 3. 无全局路径命名空间的第二实现 | 未达成 | 不变。macOS 有全局路径命名空间,且两个实现同一作者 |
| 4. conformance 覆盖 | 操作与导出面已达成 | 不变 |
| 5. 无实现需要表、注册中心或名字解析器 | 达成 | **须重新检验**:port 层会引入 fd 表与路径解析器。二者位于 musl 内部而非 openkal 实现内部,判据 5 约束的是后者,但这一区分须在文档中写明,否则该判据会被读成已被违反 |

判据 3 不因本方案而推进,这一点须明确:**musl 在 macOS 上运行是一个强结果,但它不是
判据 3 要求的那个结果。** 判据 3 要求的是一个没有全局路径命名空间的环境,
而 Linux 与 macOS 都有。

---

## 8. 现有 `openkal-libc` 包的去向

本方案实施后,现有包的 243 行分为三部分:

| 部分 | 去向 |
|---|---|
| `okc::resolve` 的最长前缀匹配 | 被 port 层吸收,成为 `open(2)` 路径解析的基础 |
| `okc::mutex` 的三态构造 | 被 musl 自己的 `pthread_mutex` 取代 |
| `okc::print` 族与文件操作 | 被 musl 的 `stdio` 取代 |

因此该包在本方案完成后不再有独立存在的理由。三条处置候选:

1. **改名并保留**为规范的两处分解的最小演示(例如 `openkal-probe`),
   与 musl 移植并存;它的价值在于短小到可以完整阅读。
2. **归档**,由 musl 移植取代。
3. **改为 musl 移植的宿主包**,即本方案的 port 层就住在这个仓库里,包名保持不变,
   内容由探针替换为 port 层。

**倾向候选 3**,理由是 `openkal-libc` 这个名字所描述的正是 port 层要做的事,
而当前内容与该名字不符正是本方案被提出的起点。若采纳,则本仓库的
`src/libc.cppm` 与 `src/libc.cpp` 在阶段 C 被 port 层替换,
`examples/wordcount` 改为纯 POSIX 源码 —— 而它改为纯 POSIX 之后仍能输出正确结果,
本身就是这次移植成立的最直接证据。

---

## 9. 待 review 的决策点

以下五点在实施前需要确认,它们各自会改变后续工作。

1. **§3.2 的 `mprotect`**:声称成功而不生效违反规范 §2.2 的判别式。
   接受该违反并记录,还是返回 `ENOSYS` 并实测 musl 的反应?
2. **§4 的符号共存**:三条对策中选哪一条?倾向对策 2(两阶段链接加符号局部化),
   但它是确定性最低的一环。
3. **§8 的包去向**:倾向候选 3(port 层住进本仓库,包名不变)。
4. **范围**:阶段 E 的 libc-test 是全量还是子集?全量会包含大量依赖 Linux 特有语义
   的项目,逐条归因的成本可能超过移植本身。
5. **停止条件**:阶段 B 的结论若为否定,是否就此停止 macOS 方向而只做 Linux?
   Linux 单平台的结果仍然满足完备性判据 1,但失去本方案最强的那个论据。
