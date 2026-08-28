# openkal-linux#13 追加反馈:三条缺陷的设计方案

2026-08-28 · 源码核查 + 设计方案

**状态:已实施(openkal-musl 0.6.0)。** §0 记录结果与方案的偏离;其余各节保留为
当初的判断记录,不作追改 —— 一份被结果修改过的方案不再能说明结果是否被预见。

核实基线:openkal-musl `93c247f`(0.5.0)、openkal-linux `baa5d93`(0.6.0)、
openkal `ffab248`(0.8)。与报告者 `yspbwx2010` 的运行环境同版本。
标「读码」的附文件行号;标「实测」的附命令与读数。

---

## 0. 结果

| 方案 | 结果 |
| --- | --- |
| 三条缺陷 A/B/C | 全部实施,判据全绿 |
| 「无流」的判定条件 | **不成立**,见 0.1。这是本方案唯一的实质性误判 |
| B 只需处理自身上下文 | **不成立**,见 0.2 |
| abort 判据分两层 | **不够**,要三层,见 0.3 |
| 方案未预见而实施中挖出的缺陷 | 三条,见 0.4 |

**A/B 对照(判据必须跨过真正变化的那段)。** 把 `port/src` 退回 `93c247f`、
只留新探针,同一命令跑一次:

```
$ git checkout -- port/src && mcpp build
$ bash tools/run-probe.sh examples/subprocess subprocess --fork --shell --abort-signal
-- failures: 7 --
```

七条红,逐条对应一个修复;十五条控制项全绿。恢复实现后 `-- failures: 0 --`。
**七条与三个缺陷的对应**:重定向四条(posix_spawn / execve / addopen / system)、
`addclose` 拒绝一条、abort 一条、WNOHANG 一条。

### 0.1 ⚠️⚠️ 「无流」不能用句柄的值判定 —— 零是一个合法的流

方案 §1.3 D1 写的是「`d->stream` 为零 ⇒ 拒绝」。**第一次运行时每一个 spawn 都
返回 EBADF**,包括不做任何重定向的控制项。

真因:`openkal-linux/src/stream.cpp:6` —— `kal_stdin()` 返回 `kal_stream{0}`。
这个后端的流**就是环境自己的描述符**,所以标准输入的句柄是 0,而 0 同时是
`kal_spawn_streams` 里「继承」的哨兵值。

⇒ 判定必须落在**种类**上而不是值上(`okm_spawn.c` 的 `stream_for_spawn`),这与
`okm_syscall.c:88` 的 `stream_of` 本来就一致 —— 我在写方案时读了那个函数却把它
读成了「值为零即无流」。

⇒ 并且由此浮出方案完全没有的一条:**规范在一个接口里保留了另一个接口会发出的值**。
`dup2(0, 1)` 之后 spawn,位置 1 要放的句柄是 0,与「继承」无法区分。按端口自己的
规矩**拒绝**(ENOSYS),记入 `musl/PATCHES.md`,并上报规范。

### 0.2 ⚠️ B 的目标标识**不能**比较,而方案说要比较

方案 §2.2 写「目标必须是调用方自身(`tid == OKM_CONTEXT_ID()`)」。读
`port/src/okm_thread.c:130` 发现:线程的 `tid` 是端口自己的计数器 `++g_tid`,
不是 `kal_task_current()`。⇒ 那个比较对**除第一个上下文以外的每一个**都会失败,
线程里的 `abort` 会退回 `hlt`。

正确的规则更简单也更对:**终止性信号的默认动作结束的是进程而不是被点名的上下文**
(Linux 上也是如此),所以目标根本不必检查。

### 0.3 ⚠️ abort 判据要三层,两层会在缺陷上变绿

方案 §2.3 的「层 1 Linux 专有 / 层 2 三系统通用(能与 exit(0) 区分)」不够:
**缺陷产生的是一个 fault,而 fault 与 exit(0) 也是能区分的**,所以层 2 在修复前
就已经绿了(A/B 实测:该条在老代码上是 `ok`)。

⇒ 每一行断言**它自己的后端产生的那一种结束**:

| 行 | 标志 | 判据 | 缺陷时的读数 |
| --- | --- | --- | --- |
| linux | `--abort-signal` | `WTERMSIG == SIGABRT` | SIGSEGV |
| macos | `--abort-status` | `WEXITSTATUS == 134` | 信号死亡 |
| windows | `--abort-terminated` | `WIFSIGNALED` 且非 fault | `WIFEXITED` |

三条在缺陷上各自为红。

### 0.4 方案未预见、实施中挖出的三条

1. **file action 的施加顺序是反的。** musl 的 `add*` 是**前插**,它自己的
   `posix_spawn` 先走到表尾再沿 `prev` 回来;这个端口沿 `next` 走,于是按**添加的
   逆序**施加。两个动作指向同一位置才看得见,而 musl 自己只发一个 `adddup2`。
2. **`kill(child, 0)` 会杀掉那个子进程。** 旧代码不看信号号,一律
   `kal_process_terminate` —— 唯一目的是「什么都不改变」的那个调用改变了一切。
3. **musl 保留 32/33/34 三个信号自用**(`pthread_impl.h:129-131`:SIGTIMER /
   SIGCANCEL / SIGSYNCCALL),都经 `tkill` 发出。若把 33 当作终止性信号,
   **第一次 `pthread_cancel` 就会结束整个程序**。三个一律 ENOSYS,与它们今天得到的
   答案相同。

### 0.5 顺带收敛的一处重复

`open` 的标志翻译表原本只在 `do_openat` 里,而 file action 的 `addopen` 需要同一张
表。抽成 `okm_open_flags`(`okm_fd.c`),两个调用者共用 —— 第二处推导会与第一处一致
到其中一处被扩展为止。

---

## 0. 一句话

> 三条缺陷是同一个形状:**原子早就在规范里,缺的是通往它的那条路**。
> 而三条共用同一条二级教训:**探针七项全绿,问的是另一个问题。**

维护者在 #13 的上一条回复里写过"规范没有变,缺的是路由"。这三条说明那句话
对**这一侧**同样成立 —— `kal_fs_stream`、`kal_abort`、`kal_timeout_wait_process`
都已在 `SURFACE.txt` 里,三处都没有被接上。

| | 缺陷 | 所需原子 | 现状 |
| --- | --- | --- | --- |
| A | 重定向不跨 spawn | `kal_fs_stream` + `kal_spawn_streams` + `KAL_PROCESS_PROP_STREAM_PASSING` | 三者俱在,`streams` 恒为 `{0,0,0}` |
| B | `abort()` 落到 `hlt`,读数为 SIGSEGV | `kal_abort`(linux 实现即 `tgkill(SIGABRT)`) | `SYS_tkill`/`SYS_tgkill` 不在派发表 |
| C | `waitpid(WNOHANG)` 阻塞 | `kal_timeout_wait_process` | `do_wait4` 丢弃 `options` |

---

## 1. 缺陷 A:重定向不跨 spawn

### 1.1 读码 —— 五链,每一环可复核

1. `port/src/okm_spawn.c:81` —— `struct kal_spawn_streams streams = { 0, 0, 0 };`
   初值从不从描述符表播种,只会被显式的 file action 覆盖。
2. `openkal/include/openkal/process.h:17-25` —— 零句柄表示被启动的程序继承其父的
   对应流。
3. `openkal-linux/src/process.cpp:84-87` —— clone 出的子进程里 `dup3` **仅对非零
   句柄发出**。零就保持 clone 继承来的 0/1/2,也就是进程真实的描述符 1。
4. `port/src/okm_fd.c:167-181` —— `dup2` 只改本库自己的表,不发也发不出系统调用。
   进程层面的描述符 1 仍是程序启动时那个流。
5. `port/src/okm_syscall.c:1203-1206` —— **`execve` 走同一条路**:
   `__posix_spawn(&child, path, /*fa=*/0, 0, argv, envp)`。`fork` 的副本明明持有
   带重定向的描述符表,这一步把它丢掉。

⇒ `posix_spawn` 无 file action 丢;`fork` + `execve` 也丢。报告者的最小复现与此
逐字对应。`popen` 之所以能过,是因为 musl 用显式 `adddup2` 走了被翻译的那条支路。

### 1.2 为什么这不是规范问题

- `kal_fs_stream`(`openkal/include/openkal/fs.h:105`)是 `openkal.fs` 的**必需**
  操作,端口已在 `okm_syscall.c:198` 把结果存进 `okm_desc.stream`。
- channel(`okm_syscall.c:614,620`)、已连接 socket(`okm_net.c:373`)、三个标准流
  (`okm_fd.c:235-237`)都已带流。
- 三个后端全部声明 `KAL_PROCESS_PROP_STREAM_PASSING`
  (linux `process.cpp:255`、macos `:251`、windows `:294`)。

### 1.3 设计

#### D1 —— 播种规则,三种情形穷举而不是举例

对 `fd ∈ {0,1,2}`,取 `d = okm_desc_of(fd)`:

| 情形 | 传给 `kal_spawn_streams` 的值 | 理由 |
| --- | --- | --- |
| `d` 存在且 `d->stream` 等于启动时记下的对应初值 | `0` | 描述符仍指着程序启动时那个流,"继承"就是它,零是每个环境都能给的答案 |
| `d` 存在且 `d->stream` 非零、与初值不同 | `d->stream` | 真发生了重定向 |
| `d` 不存在,或 `d->stream` 为零(未连接 socket、目录) | **拒绝** | 无流可放,而零会被读成"继承",那是安静地做另一件事 |

初值在 `okm_table_init`(`okm_fd.c:225-237`)里与三次 `okm_fd_bind` 同处记下:

```c
static kal_uintptr g_std_stream[3];   /* 程序启动时的 in/out/err */
kal_uintptr okm_std_stream(int fd);   /* fd ∈ [0,3) 时返回,否则 0 */
```

> ⭐ **为什么记初值而不是当场再调 `kal_stdout()`**:两者当前等价,但"描述符是否仍
> 指着程序启动时那个流"是一条关于**历史**的判断,把它表达成一次记录而不是一次
> 重新提问,是这条规则唯一说得清的形式。

#### D2 —— 留 `0` 是承重的,不是省事

第一行留 `0` 而不是无条件传显式句柄,原因是 `KAL_PROCESS_PROP_STREAM_PASSING`
存在本身:一个没有流传递的后端仍然能启动继承父流的程序。**只在真发生重定向时
才要求那条能力**,是这条设计里唯一让裸机后端不受影响的地方。

若确需放置流而该位缺席 ⇒ `posix_spawn` 返回 `ENOSYS`。理由与 `okm_spawn.c:104-108`
现有的注释同一条:把程序启动到调用方没有要求的状态,比不启动它更坏。

> ⚠️⚠️ **`kal_process_props` 是数据不是函数,弱引用为空时读它就是解引用零。**
> 端口现有 25 处弱引用全是函数(`okm_fork.c:60`、`okm_net.c:68-86` 等),测的是
> `!= 0` 后再调用;数据符号不能照抄这个写法。判据必须是 `&kal_process_props != 0`,
> 而不是 `kal_process_props != 0`。这正是 #13 里报告的那类空跳,只是发生在数据上。
> **这一条要单独写进 CI 的可选接口断言**(openkal-linux#16 建立的那组)。

#### D3 —— file actions:叠加顺序,以及两条语义要改

顺序:**先按 D1 播种,再叠加 file actions**。显式意图压过继承。

| 动作 | 现状 | 改为 | 理由 |
| --- | --- | --- | --- |
| `FDOP_DUP2`,`fd ∈ {0,1,2}` | 取 `okm_desc_of(srcfd)->stream` | 不变,但源描述符无流时拒绝而非放零 | 与 D1 第三行同一条规则 |
| `FDOP_DUP2`,`fd > 2` | `ENOSYS` | 不变 | `kal_spawn_streams` 只有三个位置,这是真拒绝 |
| `FDOP_CLOSE`,`fd > 2` | 空操作 | 不变 | 三以上本来就不被继承,**完成于无事可做** |
| `FDOP_CLOSE`,`fd ∈ {0,1,2}` | 空操作 | **`ENOSYS`** | 播种之后子进程确实继承 0/1/2,"忽略"就变成了被接受而未执行的动作 |
| `FDOP_OPEN` | `ENOSYS` | **实现** | 可表达:在此打开,取 `kal_fs_stream` 放进对应位置 |

> ⭐ `FDOP_CLOSE` 这一行是本方案里唯一一处**语义翻转**:现在注释写着"没有要求的
> 东西不会被继承",而 `{0,0,0}` 恰恰让 0/1/2 被继承了 —— 注释描述的是一个当时不
> 成立的前提。修完之后前提在 `fd > 2` 上成立,在 `fd ≤ 2` 上仍不成立,所以按 fd
> 分成两行,而不是把注释改一改留着。

`FDOP_OPEN` 的落地形状:`okm_resolve` → `okm_fs_open`(标志翻译与
`do_openat:169-179` 同一张表)→ `kal_fs_stream` → 放位 → **spawn 返回后释放该文件**。

> ⚠️ **待决 Q1(需规范澄清,不需新操作)**:调用方在 spawn 之后释放它放进去的流,
> 被启动的程序是否仍持有?`process.h:83-87` 对 channel **已经这样要求**了
> ("父方不释放 `theirs` 就永远看不到输入结束"),但没有对一般的流说同一句话。
> 建议在 `kal_process_spawn` 的注释里补一句陈述,而不是加操作 —— 不触 clause 8。

#### D4 —— 改点唯一

播种放在 `__posix_spawn` 内部,不放在调用点。于是 `execve`(`okm_syscall.c:1203`)、
`system`、`popen` 由同一处改动一并答复。**判据必须分别覆盖这三条路线**,理由见
§6:一处改动答复三个入口,不等于三个入口都有判据。

#### D5 —— 待决 Q2:`addclose(0..2)` 拒绝,还是造一个已闭合的 channel?

- **方案 a(推荐)**:`ENOSYS`。诚实,且与 `chmod` 的处置同一条理由 ——
  "拒绝"比"看起来成功而给了子进程父方的 stdin"好处理,后者能把父方的终端挂住。
- 方案 b:用 `kal_process_channel` 造一对,把远端放进去、两端立刻释放,子进程读到
  的是立即的输入结束。**不推荐**:它凭空造了一个资源来模拟"没有资源",正是
  clause 3.1 归类为模拟的那种东西。

#### D6 —— 待决 Q3:标准描述符被关闭

程序 `close(1)` 之后 spawn。musl 表里 fd 1 消失,但**进程真实的 fd 1 还开着**,
子进程仍会拿到它。openkal 没有"无流"这个值,所以这一条无法表达。
建议:按 D1 第三行**拒绝**,并把分歧记进 `musl/PATCHES.md`。

### 1.4 判据

> ⚠️ 现有 `examples/subprocess/src/main.c` 七项观察全绿,而**它从不在 spawn 之前
> 重定向父方描述符**。这次的判据必须落到"子进程写进了哪里",不是"spawn 成功了"。

新增观察(建议放进 `examples/subprocess`,与既有七项同一个二进制,三系统同跑):

| # | 观察 | 分母 / 反向对照 |
| --- | --- | --- |
| 1 | 父方 `dup2` fd 1 到文件 → `posix_spawn` → 文件字节数**恰好等于**子进程输出长度 | 同时断言父方原 stdout 收到 **0 字节** |
| 2 | 同上,改走 `fork` + `execve` | 与 1 是两个入口,不能只测一个(D4) |
| 3 | 同上,改走 `system()` | 第三个入口 |
| 4 | `addopen` 把子进程 stdout 送进具名文件 | 新实现的路 |
| 5 | `addclose(&fa, 1)` 返回 `ENOSYS` | **拒绝也是判据** |
| 6 | 不做任何重定向的 spawn 仍然继承 | 反向对照:确认零路径没被改坏 |

> ⭐ 第 1 条的"两侧"是刻意的:只断言文件里有内容,`printf` 恰好两处都写也会绿。
> 两个反向对照抓的是不同的东西。

---

## 2. 缺陷 B:`abort()` 落到 `hlt`,读数变成 SIGSEGV

### 2.1 读码 + 实测

1. `SYS_tkill`、`SYS_tgkill` 在 `port/src/` **一次都不出现** ⇒ 落到
   `okm_syscall.c:1490` 的 `default: return -ENOSYS`。
2. musl 的 `raise`(`musl/src/signal/raise.c`)整体就是
   `syscall(SYS_tkill, __pthread_self()->tid, sig)` ⇒ 返回 ENOSYS,进程不死。
3. musl 的 `abort`(`musl/src/exit/abort.c`)于是穿过 raise → rt_sigaction → tkill
   → rt_sigprocmask,走到它自己注释为 unreachable 的 `a_crash()`。
4. x86_64 的 `a_crash`(`musl/arch/x86_64/atomic_arch.h:105-109`)是
   `__asm__("hlt")`。

**实测**(2026-08-28,宿主内核):

```
$ printf 'int main(void){ __asm__ __volatile__("hlt"); return 0; }\n' > hlt.c
$ env -i PATH=/usr/bin:/bin /usr/bin/gcc -o hlt hlt.c && ./hlt; echo "exit=$?"
Segmentation fault (core dumped)
exit=139
```

⇒ **这个端口里每一次 `abort()` 都以 SIGSEGV 收场**:未捕获异常、`assert`、
`std::terminate`、`__cxa_pure_virtual`、`__stack_chk_fail`,全在内。

`SYS_kill` 虽在(`okm_syscall.c:1217-1222`)但只解析子进程,而 `getpid()` 返回 1
(`:1175`)、子进程 pid 自 1001 起(`:394`)⇒ `kill(getpid(), SIGABRT)` 报 ESRCH。

### 2.2 设计 —— 一张处置表,不是一个特例

新增 `SYS_tkill` / `SYS_tgkill`,并扩充 `SYS_kill`。**目标必须是调用方自身**
(`tid == OKM_CONTEXT_ID()`,或 `pid ∈ {1, 0, -1}`);指向别的上下文的一律
`ENOSYS` —— 这个端口没有任何办法打断另一个上下文,而假装能是 clause 3.1 的模拟。

| 信号 | 处置 | 理由 |
| --- | --- | --- |
| `0` | 返回 0 | 存在性探测,不投递 |
| `SIGABRT` | `kal_abort(msg, len)` | **openkal-linux 的 `kal_abort` 本身就是 `tgkill(pid, tid, SIGABRT)`**(`src/abort.cpp`)⇒ 得到的是**真的信号死亡**,`WIFSIGNALED` 成立,core 也在 |
| 默认动作为终止的其余信号 | `kal_exit(128 + sig)` | 与 `okm_syscall.c:1213` 已有的折叠约定同一条 |
| 默认动作为忽略(`SIGCHLD`/`SIGURG`/`SIGWINCH`) | 返回 0 | 完成于无事可做 |
| 默认动作为停止(`SIGSTOP`/`SIGTSTP`/`SIGCONT`) | `ENOSYS` | 无法表达,拒绝 |

> ⭐ 第二行是这条设计的全部价值所在,而它是**读 openkal-linux 的实现读出来的**,
> 不是设计出来的:`kal_abort` 已经在发真信号。若改成自造 `kal_exit(134)`,父方看到
> 的是 `WIFEXITED && 134` 而不是 `WIFSIGNALED && SIGABRT` —— 与 Linux 的读数不同,
> 而现在不必不同。

> ⚠️ **待决 Q4**:第三行在 macOS / Windows 后端上退化成退出码而非信号死亡。
> 判据因此要分两层写(见 §2.3),否则 Linux 之外两格会以 SKIP 或假绿收场。

与 `SYS_rt_sigaction` 拒绝真 handler(`okm_syscall.c:1474-1483`)一致:这里做的
是**默认动作**,不是投递。安装处理器仍然被拒,两条不矛盾。

### 2.3 判据

| # | 观察 | 层 |
| --- | --- | --- |
| 1 | 子程序 `abort()` ⇒ 父方 `wait` 报 `WIFSIGNALED && WTERMSIG == SIGABRT` | Linux 专有 |
| 2 | 子程序 `assert(0)` ⇒ 同上 | Linux 专有 |
| 3 | 子程序 `abort()` ⇒ 父方能把它与 `exit(0)`、与故障区分开 | 三系统通用 |
| 4 | 子程序 `exit(3)` ⇒ `WIFEXITED && 3` | **反向对照**:确认正常退出没被改坏 |
| 5 | `pthread_kill(其他线程, SIGTERM)` ⇒ `ENOSYS` | 三系统通用,拒绝也是判据 |

> ⚠️ 判据 1/2 不能写成"退出码不是 139"。139 在修好之后仍然是一个合法读数
> (真的段错误)。**判据是 `WTERMSIG == SIGABRT`,不是"不等于某个值"。**

---

## 3. 缺陷 C:`waitpid(WNOHANG)` 阻塞 —— 原子也已在

`do_wait4`(`okm_syscall.c:434-436`)`(void)options`。而
**`kal_timeout_wait_process` 已在 openkal 0.8 的 `SURFACE.txt:113`**。

设计:`options & WNOHANG` ⇒ `kal_timeout_wait_process(h, OKM_NOW_NS, ...)`,
`kal_err_again` ⇒ 返回 0(WNOHANG 的"尚无子进程可收"就是这个答案)。
`OKM_NOW_NS == 1`(`okm.h:271`)是端口已有的约定,`do_read`/`do_write` 的
`O_NONBLOCK` 路径用的就是它。

> ⚠️ `timeout.h:12-14` 明说**零表示不设界**,所以 `WNOHANG` 不能传 0;传 1 会被
> 实现向上舍到 `kal_timeout_granularity_ns`。分歧("WNOHANG 至多阻塞一个时钟粒度")
> 记进 `musl/PATCHES.md`。
> `openkal.timeout` 是可选接口 ⇒ 弱引用 + 空判,缺席时 `WNOHANG` 报 `ENOSYS`。

判据:父方启动一个先睡后退的程序,`waitpid(WNOHANG)` 在其存活期间返回 0 且
**父方在这段时间里完成了至少一次别的可观测动作**(证明它没被挡住),随后阻塞式
`waitpid` 取回状态。反向对照:不带 `WNOHANG` 的同一次等待仍然阻塞并拿到状态。

---

## 4. 新增能力:`default:` 支的诊断通道

`okm_syscall.c:1490` 静默返回 ENOSYS。消费者要知道程序需要而没有的是哪个操作,
目前唯一的办法是读 `okm_syscall.c` —— 这不是一件可以要求消费者做的事。#13 里
两轮往返、以及本文档第 8 节两条仍未决的项,大部分成本都来自这里。

设计:

- 形状:**环境变量**,`OPENKAL_MUSL_TRACE=enosys`。不用构建特性 —— 消费者是在
  已有的二进制上撞见的,重建 C 库正是要消除的代价。
- 只覆盖 `default:` 支。像 `mprotect`(`:1061-1070`)那样**刻意**返回 ENOSYS 的
  分支不进跟踪:那是决定,不是缺口。这条区分要写进注释。
- 写法:直接 `kal_stream_write` 到 stderr,不经 musl 的 stdio —— 失败的调用本身
  可能就在 stdio 里面;不分配。
- 每个号码只印一次(小的已见集合),并在文档里写明"只印一次",否则重试循环会淹掉
  输出而读者以为只发生了一次。

判据(两侧):

1. 设了变量,程序调 `symlink` ⇒ stderr 出现命名该号码的一行。
2. **没设变量,同一程序 ⇒ stderr 收到 0 字节**(超出程序自身输出之外)。
3. 同一号码被调用 100 次 ⇒ 恰好 1 行。

---

## 5. 文档:`README.md` 分歧表补两行

现表(`README.md:111-118`)记了 `chmod`/`chown` 与符号链接,没有记:

1. **`open` 与 `mkdir` 的 `mode` 参数被丢弃**(`okm_syscall.c:144`)。
   `mkdir(path, 0700)` 报成功而 `stat` 读回 0777 —— 这正是 `okm_opt.h` 开头立下的
   规矩("下面没有任何东西在什么都没做的情况下报告成功")所禁止的形状。逐个拒绝
   非 0777 的 mode 不现实,所以答案是**记录**,与 `chmod` 那一行并列。
2. **`WNOHANG` 至多阻塞一个时钟粒度**(§3)。

同时给报告者的替代物已在 #13 的回复里写明:在这个平台上"只有我能读"不由 mode 位
承载,由环境供给了哪些预开目录承载(`okm_fd.c:18-22`)。

---

## 6. 判据总表与两条元规则

| 缺陷 | 观察数 | 反向对照 | 层 |
| --- | --- | --- | --- |
| A 重定向 | 6 | 第 6 条(零路径未坏) | 三系统 |
| B abort | 5 | 第 4 条(正常退出未坏) | 1/2 Linux 专有,3/4/5 三系统 |
| C WNOHANG | 2 | 阻塞式等待仍正确 | 三系统,可选接口缺席时 SKIP 要显式 |
| 诊断通道 | 3 | 第 2 条(未开启时零字节) | 三系统 |

两条元规则,是这批缺陷本身教出来的:

> ⭐ **一处改动答复三个入口,不等于三个入口都有判据。** A 的改点只有一个
> (`__posix_spawn` 内部),但 `posix_spawn` / `execve` / `system` 是三条独立的
> 调用链,必须各测一条。
>
> ⚠️ **探针的绿必须能回答消费者问的那个问题。** `examples/subprocess` 七项全绿,
> 而 #13 报的那件事它一次都没问过。新增观察之前先检查:这条观察若被删掉,哪一条
> 缺陷会重新变成绿的?答不上来的观察不要加。

---

## 7. 明确不做

- **不实现 `chmod` / `symlink`。** #13 里的拒绝理由与代码一致,报告者已接受。
- **不给 openkal 加权限操作。** 理由不变(FAT / ESP / Windows ACL 无共同模型,
  clause 6.4 排除)。
- **不做信号投递。** 本方案只做**默认动作**,不做异步交付;
  `rt_sigaction` 拒绝真 handler 的处置保持不变。
- **不给 `kal_spawn_streams` 加第四个位置**,也不加"无流"的值。D5/D6 两处待决都按
  拒绝处理,分歧入 `PATCHES.md`。

---

## 8. 未决:等报告者的现场数据

两条不在本方案内,因为源码给不出判据,已在 #13 的回复里逐条列了要什么:

1. **递归复制的 EAGAIN。** 复制路径上没有任何一处能产生 EAGAIN
   (`copy_file_range` 是纯读写循环,`okm_syscall.c:628-693`;openkal-linux 的
   `fs.cpp` 里没有 `kal_err_again`)。端口里 EAGAIN 只有三个产地,领先假说是
   **子进程表 64 槽满**(`okm_syscall.c:392-408`,仅由 `wait4` 回收)。
   ⇒ 若坐实,是第四条缺陷,且与 §3 的 `WNOHANG` 同一片区域。
2. **启动段错误 rip = 0。** 报告者的两条排除已复核成立
   (`rt_sigaction` `okm_syscall.c:1474-1483`;25 处弱引用逐条有空判)。
   ⚠️ §2 的结论**否掉了**报告者"139 与空跳是同一件事"的相关性猜测:`hlt` 会把
   指令指针留在那条指令上,不是 0,所以是两件事。
   已请求的决定性观察:故障时的 `x/4gx $rsp` —— 栈顶那个字就是通过零调用的那个
   调用方的返回地址。

> ⚠️ **两条都不该在 §1–§4 落地之后被当成"顺带就好了"。** 修好硬失败会暴露下一条,
> 而 §2 一修,"SIGSEGV"这个读数的含义就变了 —— 这两条要在修完之后**重测一遍**
> 才谈得上归因。
