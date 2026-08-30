# openkal-linux#13:一次覆盖完的设计方案

2026-08-30 · 全面排查 + 设计 · 待 review,尚未实施
前置文档:`2026-08-30-issue13-exec-search-and-what-it-hid.md`(A/B/C/D/E 的定位过程)

核实基线 —— **与报告者同版本**:openkal-musl **0.9.0**(`aab97bc`)、
openkal-linux **0.7.1**、openkal-llvm-runtime 0.5.0,目标 `x86_64-linux-musl`。
**每一条都有实测读数和宿主 glibc 对照**;只有读码没有读数的,本文明确标出。

---

## 0. 这一轮为什么要扩大范围

上一份文档只回答了报告者提到的东西。但他的十九条失败里有两条**他自己没定位、
我们也没定位**,而「等下一轮反馈」这个做法本身有问题:**报告者只能报他撞到的,
撞不到的会在下一个版本继续埋着**。上一轮的教训正是这个——
`examples/subprocess` 七条全绿,而它问的根本不是消费者会问的问题。

所以这一轮换了做法:**写一个宽面探针,把一个「带终端界面 + 会起命令 + 重度用
文件系统 + 起线程」的程序会碰的 POSIX 面铺开跑一遍**,和宿主逐条对照。

结果:**26 项里 13 项失败,其中 11 项与宿主不一致**——另外 2 项(`TIOCGWINSZ`、
`tcgetattr`)两边同样失败,只是因为输出被重定向所以不是终端,不算。
再加一项**「通过了但答案不同」**:`sysconf(_SC_NPROCESSORS_ONLN)` 这里答 1、宿主答 32,
它不会失败,只会让线程池按 1 开(F4)。

其中**七条是「接受了、没执行、报成功」**——这个端口反复声明要拒绝的那个形状。

⚠️ **最重的一条不在报告者的清单里:文件锁形同虚设。** 两个进程可以同时持有同一把
排他锁,而且 `F_GETLK` 报告的锁状态**与事实相反**。

---

## 1. 用户目前还剩哪些问题

报告者第五轮:107 项里 19 红,外加 4 段被静默跳过。逐条归位:

| 数量 | 现象 | 归属 | 本方案 |
| --- | --- | --- | --- |
| 9 | 需要真实子进程输出/退出码的全部拿到 127 | **A**(+B) | §3.1 |
| 4段 | `bwrap` 按裸名探测得 127,判定「未安装」 | **A** | §3.1 |
| 7 | 限制性 mode / 可执行位 / 写后 chmod | **使用侧**,双方已达成一致 | 不做 |
| 1 | 目录 `last_write_time` 报 EISDIR | **D**(且报告者归因反了) | §3.4 |
| 1 | 后台任务永不终结 | **未确证**,三个候选机制 | §5 |
| 1 | 一处 hang | **未确证**,首选变成 F2 | §5 |

⇒ **修完 A+B+D,报告者当下的 19 条里 10 条转绿,4 段跳过恢复,7 条是他自己的。**
剩下 2 条见 §5——本轮把候选机制从「说不出」收敛到三个可证伪的,且其中两个
(C、F2)**顺手就修掉了**,所以很可能不需要再问他。

---

## 2. 全部缺陷一览(按形状分组,不按发现顺序)

### 组一:接受了、没执行、报了成功 —— 必须修

| # | 缺陷 | 读数 | 宿主 |
| --- | --- | --- | --- |
| **F1** | `fcntl(F_SETLK)` 返回 0 而**不加锁** | 两个进程同时持有排他锁 | 第二个得 EAGAIN |
| **F2** | `fcntl(F_GETLK)` 返回 0 而**不写 `l_type`** | 调用者读回 `F_WRLCK`,与事实相反 | 写回 `F_UNLCK` |
| **F3** | `getppid()` 把 `-38` 当 pid 返回,`errno` 不动 | `getppid()=-38` | `=1416476` |
| **B** | `posix_spawnp` 报成功但**从不搜 PATH** | `ok, exit=127` | `exit=7` |
| **C** | `kill()` 打不到 `fork`+`execve` 起的程序 | 程序跑完全程,父亲读到「死于 SIGTERM」 | 真的被杀 |
| **F5** | `access(path, X_OK)` 对任何存在的名字都答「可执行」 | X 位为空的文件答 **0** | 答 `EACCES` |
| **F6** | `F_SETFD`/`FD_CLOEXEC` 被记录但**无效** | fd>2 根本不跨 spawn(子进程 `Bad file descriptor`) | fd 4 跨过去了 |
| **F7** | `sigaltstack()` 报成功而**什么都没装** | 装完再查:`ss_sp=0 ss_size=0` | `ss_sp=0x4040a0 ss_size=65536` |

### 组二:诚实地缺席,但**答得出却没答**

| # | 缺陷 | 读数 | 本可以答 |
| --- | --- | --- | --- |
| **F8** | `getrlimit(RLIMIT_NOFILE)` / `sysconf(_SC_OPEN_MAX)` | ENOSYS / **0** | 端口自己就是 `OKM_MAX_FD=1024` |
| **E** | `setsid` / `setpgid` 报 ENOSYS | ENOSYS(38) | 与 `getpgid`/`getsid` 的回答矛盾,§3.5 |

### 组三:诚实地缺席,规范里确实没有原子 —— 记录 + 上报

| # | 操作 | 规范核查 |
| --- | --- | --- |
| **F4** | `sched_getaffinity` → `hardware_concurrency` **静默答 1**(宿主 32) | `task.h` 只有 `KAL_TASK_PROP_PARALLEL`,**没有数量** |
| F9 | `statvfs` → `fs::space` | `fs.h` 无卷容量操作 |
| F10 | `link` → `fs::create_hard_link` | `kal_fs_link_create` 是符号链接,无硬链接 |
| F11 | `mkfifo` / `mknod` | 无原子 |
| F12 | `flock` | 无原子(与 F1/F2 相关但不同族) |
| F13 | `socketpair` | `kal_process_channel` 是**单向管道**,见 §3.10 |

### 组四:上一份文档已定位

**A**(exec 搜索,§3.1)、**D**(目录 mtime,§3.4)。

---

## 3. 逐条设计

### 3.1 A —— `execve` 起不来程序时必须返回

设计见前置文档 §1.5,此处只记**改点清单**与两处补充:

- `okm_spawn.c` `__posix_spawn`:`okm_resolve` 之后加一次 `kal_fs_info` 前置检查,
  `kal_node_absent → ENOENT`、`kal_node_directory → EACCES`;走既有的 `refused`
  收尾路径(`:314-318`),不要另开 return。
- 前置检查写成 `startable()` 小函数,`_WIN32` 的 `.exe` 重试(`:342-357`)复用它,
  否则那条路再也走不到。
- ⚠️ **残留**:名字在但不可执行,前置检查放行,仍以 127 结束调用者。openkal 没有
  可执行位——这和 **F5** 是同一个缺口的两面,两处要一起记进分歧表。
- 完整修法在 openkal-linux(CLOEXEC 回报管道),另开 issue,**不阻塞本次发布**。

### 3.2 B —— `posix_spawnp` 要真的搜 PATH

- 仿 `okm_spawn.c` 替换 `posix_spawn.c` 的先例,再替换一份 `posix_spawnp`:
  名字含 `/` 直接调 `__posix_spawn`;否则按 PATH(缺省
  `"/usr/local/bin:/bin:/usr/bin"`,与 musl 一致)逐项试,
  `ENOENT`/`ENOTDIR` 继续,`EACCES` 记住,其余立即返回。
- `__posix_spawn` 对**不认识的 `attr->__fn` 返回 ENOSYS**,不再静默忽略。
- 依赖 A:没有 A,循环第一项就停。

### 3.3 C —— `kill` 打不到 `fork`+`execve` 起的程序

⚠️ **本仓今天修不完,而且现在有证据说明为什么。**
`openkal-linux/src/process.cpp` 的 `kal_process_terminate` 是
`kill(pid, SIGTERM)` ——**单个 pid,不是进程组**;而中间那个等待者阻塞在
`kal_process_wait` 里,这个端口没有信号投递,它跑不了任何转发代码。

#### 3.3.0 这是规范的缺口,不是后端的缺陷

⭐ **`kal_process_terminate` 没有做错任何事。** 它被要求终止某个被起的程序,它就
终止了那一个。规范说的就是这个,后端做的就是这个。缺的是**一种表达不出来的意图**:
「这个程序是我为了表达『替换我自己』而起的,它的寿命应当以我为界」。openkal 今天
没有任何原子说得出这句话。

⇒ **C 是规范缺口。** 而且更准确地说,出问题的是规范(以及本仓文档)当初下的那个
判断——`README.md:315` / `musl/PATCHES.md:114` 声称 spawn+wait 与替换映像
「a caller cannot distinguish」。**这个判断本身是错的**,而它是整条链的起点:
接受了这个判断,就没人再去找可观察的差别。目前已知两处:
**A(exec 失败时)** 与 **C(信号能不能打到)**。

⚠️ **和 A3 要分清,那一条恰恰相反,是后端缺陷。**
`kal_process_spawn` 的子进程**已经知道** `execveat` 失败了(它紧接着
`exit_group(127)`),却没有把这件事回报给父亲;而 `kal_err_not_found` 这个值早就
存在,本端口的 `okm_spawn.c:342` 还专门为它写了一条分支。
⇒ **后端手上有信息而没有交出来 = 后端缺陷;规范里没有词可以说 = 规范缺口。**
两条分别报给两个仓库,不要混成一条。

本轮做三件事:

1. **改文档**:`README.md:315` 与 `musl/PATCHES.md:114` 那句
   「a caller cannot distinguish」是错的,A 与 C 都是反例。
2. **加判据**把当前行为钉住(§4),否则修好了也没人知道。
3. **上报**:向 openkal 要一个「寿命受调用者约束」的起法。
   映射:Linux `PR_SET_PDEATHSIG`、Windows job object、macOS `kqueue`/`NOTE_EXIT` 看门狗。

> ⚠️ 排除一个看似可行的替代:让 `kal_process_terminate` 杀进程组。要让它成立,
> `kal_space_start` 得给复制出的 space 开新组——而**新组会脱离终端的前台组**,
> 一个带终端界面的程序里,任何读终端的子上下文会拿到 SIGTTIN 而停住。
> **换了一个更难查的错,不是修好。**

4. **给使用侧的当下规避**(实测有效):需要能 `kill` 的地方用
   `posix_spawn` / `system` / `popen`,不要用 `fork`+`execve`。

### 3.4 D —— 目录的修改时间

- 实测:**读是好的**(`stat(dir)` 正常),坏的是**写**;libc++ 两个重载共用
  `"last_write_time"` 这个报错名,报告者据此归因反了。
- 实测:**身下做得到** —— `kal_fs_open(dir, KAL_OPEN_READ)` 成功,
  `kal_fs_set_modified` 成功,目录时间真的改掉了。挡路的是端口无条件要
  `READ|WRITE`(`okm_syscall.c:1116`),对目录 → `O_RDWR` → EISDIR。
- **设计(推荐 D1)**:`SYS_utimensat` 先 `kal_fs_info` 问种类;是目录就退到
  `KAL_OPEN_READ`;失败如实上报。
  - 这不是模拟:调用者要的效果实实在在发生,做不到的实现会返回错误而我们照实翻译。
    与 `chmod` 的情形**不同**——`chmod` 被拒是因为会「报成功而做了别的事」。
  - ⚠️ 踩在 `fs.h:273` 的前置条件之外(「shall have been opened with
    KAL_OPEN_WRITE」),**必须记进分歧表并上报规范**(要一个目录形式,或把
    `kal_fs_set_modified` 改述在名字上)。openkal-windows 很可能做不到,那里如实失败。
- **备选 D2(纯粹派)**:报 `ENOSYS` 而不是 `EISDIR`。`EISDIR` 在说「你传错了类型」,
  调用者会去查自己的代码;`ENOSYS` 在说「这个环境没有这个操作」。
  代价:一个每个身下环境都做得到的普通操作就此长期不可用。

**请 review 定 D1 还是 D2。** 其余各条我按推荐值写。

### 3.5 E —— `setsid` / `setpgid` 与本端口自己的回答矛盾

`okm_syscall.c:1857-1876` 已经想清楚一半:没有组也没有会话,所以
`getpgid`/`getsid` 诚实回答「就一个程序,它自成一组」(实测
`getpid=1 getpgid(0)=1 getsid(0)=1`)。然后接着拒绝 `setpgid`/`setsid`,
理由是「造一个组和身处一个组不是一回事」。

⚠️ **但这两个调用问的不是「造一个组」:**

- `setpgid(0, 0)` 请求的状态是「调用者自成一组」——**按上面三行读数这已经成立**。
  ⇒ 返回 **0**。这不是报告一个不存在的效果,是报告一个**已经存在**的效果。
- `setsid()` 在 POSIX 下有成文失败:**调用者已是进程组组长时返回 EPERM**,
  而 `getpgid(0)==getpid()` 正是这个断言。⇒ 返回 **EPERM**,是真话。
- `setpgid` 指名其它任何东西 → **EPERM**。

**为什么这不只是好看**:守护化代码普遍处理 `EPERM`(`fork`-然后-`setsid` 这套舞步
就是为它存在的),**没有一份处理 `ENOSYS`**。成文的失败调用者接得住,陌生的接不住。
顺带消掉报告者 trace 里 16 行(`setpgid` 12 + `setsid` 4)。

### 3.6 ⚠️⚠️ F1 / F2 —— 文件锁形同虚设(本轮最重的一条)

#### 读码

`okm_syscall.c:1401`,一行:

```c
case F_SETLK: case F_SETLKW: case F_GETLK: return 0;
```

#### 实测(两个进程,一个锁文件,带宿主对照)

```
=== openkal-musl 0.9.0 ===
parent  F_SETLK(F_WRLCK) -> 0 errno=0 ACQUIRED
parent  F_GETLK -> 0  l_type now = 1  (F_UNLCK=2 F_WRLCK=1)
child   F_SETLK(F_WRLCK) -> 0 errno=0 ACQUIRED
=> BOTH processes hold the exclusive lock  <-- 锁什么都没做

=== 宿主 glibc 对照 ===
parent  F_SETLK(F_WRLCK) -> 0 errno=0 ACQUIRED
parent  F_GETLK -> 0  l_type now = 2  (F_UNLCK=2 F_WRLCK=1)
child   F_SETLK(F_WRLCK) -> -1 errno=11 Resource temporarily unavailable
=> only one holds it (correct)
```

两条,不是一条:

- **F1**:排他锁不排他。任何用文件锁保护写入的程序(sqlite、状态文件、
  「同一时间只跑一个实例」)在这里**没有保护而不自知**。
- **F2 更隐蔽,而且方向是反的**:POSIX 说 `F_GETLK` 在**没有**锁挡路时把
  `l_type` 写成 `F_UNLCK`。这里 `l_type` 一动不动,调用者惯例是调用前填
  `F_WRLCK`,于是**读回 `F_WRLCK`,结论是「有人持锁」**——永远。
  一个「等到锁释放为止」的循环**永不退出**。

#### ⭐ 「按理应该支持得了吧?」—— 对,而且这一条**不该学 `chmod` 长期拒绝**

先把结论摆清楚,因为它和 `chmod` 是**两种完全不同的情形**:

| | `chmod` | 文件锁 |
| --- | --- | --- |
| 三个环境能不能做? | **不能**。FAT 卷、UEFI 分区、Windows ACL 不共享一个模型 | **都能**。Linux/macOS `fcntl(F_SETLK)`、Windows `LockFileEx`,**都带字节范围** |
| 拒绝的性质 | 永久,clause 6.4 | **临时**,只是规范里还没有这个词 |

⇒ **正确的答案不是「拒绝」,是「向规范要一个原子」**,而且它的可采纳性论证
**可以逐字照抄链接那一条**(`fs.h:285-293`):

> 「一个在**资源之间**变化的性质,既不能是一个接口也不能是一个词,而要由一个
> **接收该资源的询问**来回答——那就是 `kal_fs_props`,这正是这些操作可采纳的
> 原因:一个在场但此处执行不了的操作不是 clause 6.2 的缺陷,因为调用者可以先问。」

链接就是这样进来的(`KAL_FS_PROP_LINKS` / `KAL_FS_PROP_MAKE_LINKS`)。锁的情形一模
一样:一个卷有锁、另一个没有(UEFI、只读介质)。⇒ 提案:

```c
#define KAL_FS_PROP_LOCKS  ((kal_uintptr)1u << 5)
/* 加锁在打开的文件上,和 kal_fs_set_modified 同理:名字可能已经指向别的东西。 */
int kal_fs_lock(struct kal_file, kal_u64 start, kal_u64 len, kal_uintptr mode);
int kal_fs_unlock(struct kal_file, kal_u64 start, kal_u64 len);
```

⭐ **关键的一点:实现放在身下,「持有者死了就释放」就是白拿的**——三个环境的内核
都自带这条。而这恰恰是端口自己造不出来的那一条(见下)。

#### 为什么**不能**在端口里模拟

我认真评估了 sidecar 锁文件方案(用 `KAL_OPEN_EXCLUSIVE` 做原子创建),四条否决,
**第三条是决定性的**:

1. **机械上就够不着**。`fcntl` 拿到的是 fd,而 `struct okm_desc`(`okm.h`)
   **只对目录保留路径**(`path_slot`,目录名池的下标);普通文件的描述里
   没有名字。要拼出 sidecar 的名字,得先给每一个打开的文件都留一份路径。
2. **它会在用户的名字空间里凭空造文件**。sidecar 会出现在 `readdir` 里、
   出现在 `remove_all` 里、出现在校验和里、出现在报告者自己的目录清单里。
   一个 C 库不该往调用者的目录树里放东西。
3. ⚠️⚠️ **没有崩溃恢复,而这是致命的。** 内核锁由内核在进程死亡时释放;sidecar
   没有人释放。**一个持锁时段错误的程序会把自己永久锁死**,而且下一次运行看到的
   只是「打不开」。这个端口自己的历史里就有段错误的程序。
   ——加「陈旧超时」能绕过,但**一个 C 库没有资格替调用者选那个秒数**
   (报告者的 mkdir 锁自己选了一个,那是应用的权利,不是 libc 的)。
4. **语义还差得远**:字节范围的分裂/合并、「关闭该文件的任意一个 fd 就释放全部锁」、
   「锁不跨 fork 继承」。做全了就是在 libc 里写一个锁管理器。

**同样不推荐**端口内的锁表(按 `KAL_INFO_IDENTITY` 的 `st_dev`/`st_ino` 键控):
锁在**一个程序内部**有效、**跨程序**无效,而调用者分不清自己在哪种情形——
比 ENOSYS 更坏,因为它把一个响亮的缺席换成了一个安静的半真。

#### 设计:过渡期 ENOSYS,而它是**可逆的**

- **F1 `F_SETLK`/`F_SETLKW` → `ENOSYS`**,记进分歧表,并**在记录里指明这是等规范
  的临时状态,不是 `chmod` 那种永久拒绝**。原子落地后,同一批调用点直接亮起来,
  消费者一行都不用改。
- **F2 `F_GETLK` 无论如何都要修**:要么一起 ENOSYS,要么至少把 `l_type` 写成
  `F_UNLCK`。「假装没有锁」是自洽的;「假装永远有锁」不是——后者让等锁的循环
  **永不退出**。
- ⚠️ **代价,以及消费者可以怎么办**:sqlite 拿到 `ENOSYS` 会 `SQLITE_IOERR_LOCK`
  而拒绝打开。**它有出口**:URI 参数 `nolock=1`,或 `unix-none` VFS。
  单实例守卫、状态文件互斥这类用法则要改用报告者已经在用的那种 `mkdir` 协议。
  ⇒ 这是从**静默的数据损坏**换成**响亮的打不开加一个成文的出口**,方向对。
- **这条仍请 review 拍板**:是接受 ENOSYS(推荐),还是暂时只修 F2、把 F1 留在
  `return 0` 再等一版。两者的差别是「今天能跑的某些程序会停下来」。

### 3.7 F3 —— `getppid` 把 `-38` 当 pid 返回

#### 这是已经修过一次的那个缺陷,漏了一个

`okm_syscall.c:1857` 的注释记着 `getpgrp` 的教训:musl 的 `getpgrp` 是
`return __syscall(SYS_getpgid, 0);`,**没有 `__syscall_ret`**,因为 POSIX 说它不会
失败。于是 default 支的 `-ENOSYS` 被当成进程组返回。

`musl/src/unistd/getppid.c` 是同一个形状:

```c
pid_t getppid(void) { return __syscall(SYS_getppid); }
```

实测:`getppid()=-38`,`errno` 不动。**同一个文件、同一段推理、同一族,修了一个漏了一个。**

#### 设计

`case SYS_getppid: return 0;`

- 返回 **0** 而不是 1:`getppid()==1` 在 Linux 上意味着「我的父亲死了,我被 init
  收养」,有程序据此去做守护化;返回 0 是 Linux 上 init 自己的答案,
  意思是「这个环境没有可命名的父亲」,不会触发那条路径。
- 关键的是**不能再返回 -38**。

⭐ **并且要做一次同族普查**:musl 里所有**不经 `__syscall_ret`** 的调用点,
在 default 支下都会把 `-38` 当结果交出去。这是一类而不是一个,§4 的判据里
单列一条。

### 3.8 F5 / F6 —— 两条只能记录的

- **F5 `access(X_OK)`**:openkal 没有可执行位,「存在即可执行」是唯一答得出的答案。
  实测:一个 `st_mode` 里 X 位为空的普通文件,`access(X_OK)` 答 **0**;宿主答
  `EACCES`。(目录两边都答 0,那是对的。)
  与 **A2 的残留是同一个缺口的两面**,一起记 —— 而且**这两处会互相掩护**:
  程序常先 `access(cand, X_OK)` 挑一个程序再去起它,这里两步都答「可以」,
  于是错误一路推迟到子进程的 127。
- **F6 fd>2 不跨 spawn**:`kal_spawn_streams` 只有三个位置,更多的位置**不可表达**。
  连带后果:`F_SETFD`/`FD_CLOEXEC` 被记录但不起作用(端口里 `cloexec` 只被
  `F_GETFD` 读回,`okm_spawn.c` 从不查它)。
  实测:子进程 `echo >&4` 得到 `Bad file descriptor`,宿主上正常。
  ⇒ 记录 + 上报(要一个「把这个流放到第 N 个位置」的一般形式)。
  ✓ 端口对 `adddup2(fd>2)` **已经**返回 ENOSYS(`okm_spawn.c:243`),两处一致,
  只有**隐式继承**这一半是丢的。

### 3.9 F7 / F8 —— 两条小的

- **F7 `sigaltstack` 报成功而什么都没装**:实测装完再查,`ss_sp=0 ss_size=0`
  (宿主 `ss_sp=0x4040a0 ss_size=65536`)。⚠️ **查询这一半也是虚构的**:它返回 0 并
  交出一个全零的 `stack_t`,而不是「没有装过」。这个环境没有信号,备用栈没有意义,
  按本端口自己的规矩改成 **ENOSYS**。风险低(libc++/libunwind 不把失败当致命)。
- **F8 `getrlimit(RLIMIT_NOFILE)` / `sysconf(_SC_OPEN_MAX)`**:
  ⚠️ 现在 `sysconf(_SC_OPEN_MAX)` 返回 **0**,而程序会拿它去循环关 fd、定尺寸。
  **端口自己就知道答案**:`OKM_MAX_FD = 1024`(`okm.h:83`)。
  ⇒ `case SYS_getrlimit/prlimit64`:`RLIMIT_NOFILE` 答 1024;其余仍拒绝。
  一并把 `OKM_MAX_CHILD = 256` 写进 README 的界限表(已有 fd/open-description 两行)。

### 3.10 F4 / F9-F13 —— 规范里真没有原子的

一律**如实 ENOSYS + 记进分歧表 + 上报规范**,不在本仓造轮子:

| 操作 | 记录要点 | 上报 |
| --- | --- | --- |
| **F4** `sched_getaffinity` | ⚠️ 后果是**静默的**:`hardware_concurrency()` 答 1(宿主 32),线程池按 1 开 | `openkal.task` 加一个处理器数量的询问,紧挨 `KAL_TASK_PROP_PARALLEL` |
| F9 `statvfs` | `fs::space()` 不可用 | `openkal.fs` 加卷容量询问 |
| F10 `link` | 只有符号链接,没有硬链接 | 与 `symlink` 那条合并上报 |
| F11 `mkfifo` | 无 | 低优先 |
| F12 `flock` | 与 F1/F2 一起记,读者会一起找 | 低优先 |
| F13 `socketpair` | `kal_process_channel` 是**单向管道**;双向要一个持两条流的描述符种类,而且**跨不了 spawn**(一个位置只放一条流) | 低优先 |

> F13 若将来要做:新增 `OKM_PAIR` 种类,由两次 `kal_process_channel` 组成,
> 读取走一条、写入走另一条;`stream_for_spawn` 对它返回 `ENOSYS`,因为一个
> spawn 位置放不下两条流。**本轮不做**,因为它今天是响亮的缺席而不是错答案。

---

## 4. 判据(与修复同批,缺一条都不算修完)

⚠️ **现有 `examples/subprocess` 正是漏掉这一整族的那个**:它从不按裸名字起程序、
从不 `kill` 一个 `fork`+`execve` 起的程序、从不加锁、从不问自己的身份。

| # | 判据 | 对应 |
| --- | --- | --- |
| 1 | `execvp("sh")` 在 `PATH=/nope:<真目录>` 下起得来 | A |
| 2 | `execve("/不存在")` 返回 -1 且 `errno==ENOENT`,**调用者活着** | A |
| 3 | `execve("<目录>")` 返回 -1 且 `errno==EACCES` | A |
| 4 | `posix_spawnp("sh")` 起得来;`posix_spawnp("/不存在")` 返回 ENOENT | B |
| 5 | ⭐ `fork`+`execve` 起的程序被 `kill` 后**确实停了**(用它写不出的 marker 判) | C |
| 6 | 目录 `last_write_time` 设得上(D1)/ 报 ENOSYS(D2),二选一钉死 | D |
| 7 | `setpgid(0,0)==0`;`setsid()==-1 && errno==EPERM` | E |
| 8 | ⭐ 两个进程,第二个 `F_SETLK` **拿不到**锁(或两个都拿到 ENOSYS) | F1 |
| 9 | ⭐ 无人持锁时 `F_GETLK` 把 `l_type` 写成 `F_UNLCK`(或返回 ENOSYS) | F2 |
| 10 | ⭐ `getppid() >= 0` | F3 |
| 11 | ⭐ **同族普查**:musl 里每个不走 `__syscall_ret` 的调用点都不返回负的 errno | F3 类 |
| 12 | `sysconf(_SC_OPEN_MAX) > 0` 且等于 README 记的界限 | F8 |
| 13 | `sigaltstack()` 返回 ENOSYS | F7 |
| 14 | `fork` 的复制自称的标识 == 父亲拿到的那个,且嵌套复制也各自成立 | §6 |

**上面 1-14 是「缺陷判据」:在缺陷上必须红。** 下面两条不是,它们在缺陷上也是绿的:

| # | **回归护栏**(两边都必须绿,红了说明修复弄坏了别的) | 对应 |
| --- | --- | --- |
| G1 | ⭐ **复制里的 `abort()` 仍是 SIGABRT**,`kill(getpid(),0)` 仍返回 0 | §6 的 ③ —— 改标识会牵动 `signal_self` 那条路 |
| G2 | `fork` 失败时表槽被退还(连续失败不会耗尽表) | §6 的 ② —— 表槽提前占用引入的新失效模式 |
| G3 | `examples/subprocess --fork --shell --abort-signal` 与 `examples/posix` 全绿 | 全部 |
| G4 | **控制项**:每条判据都在宿主目标上跑同一份源码 | 全部 |

⚠️ **A/B 对照是必须的**:把 `port/src` 退回 `aab97bc` 只留新探针,**1-14 必须红,
G1-G4 必须绿**。一条在缺陷上就是绿的判据证明不了任何事——上一轮的自我 review 已经在
这上面栽过一次。⭐ 而**把护栏和判据分开列**正是为了不再栽第二次:
§6 实测时 7 条里有 5 条两边都绿,它们是护栏而不是成绩。

⭐ **并且要记一笔**:这一族**没有一条**能被 `OPENKAL_MUSL_TRACE=enosys` 看见,
因为它们不是缺失的操作,而是**在场却答错的操作**。上一轮把诊断通道当成「下一轮更
便宜」的答案,这一轮证明它只覆盖了一半。是否加一个「答案可疑」的 trace 位,请 review。

---

## 5. 报告者剩下那两条:候选收敛到三个,且其中两个本轮会消失

上一轮说「从源码到不了」。现在有三个**可证伪**的机制:

| 候选 | 解释力 | 本轮是否消失 |
| --- | --- | --- |
| **F2** `F_GETLK` 永远报「有锁」 | ⭐ 最契合那处 hang:**无子进程、无缺失系统调用、无输出** 三条全中 | ✅ 修掉 |
| **C** 孤儿仍持有输出端,EOF 永不到来 | 契合「后台任务不终结」:超时 kill 之后程序还活着 | ❌ 需后端 |
| **pid 恒为 1** | 实测:父、fork 子、被起的程序 `getpid()` **全是 1**。若监督方从 pidfile 读 pid 再 `kill(pid,0)` 轮询,`kill(1,0)` 永远答「活着」 | ❌ 需设计,§6 |

⇒ **建议:先发修复,再问。** F2 修掉之后那处 hang 若消失,就不必再往下查。
若仍在,要的观察只有三个:

1. 那条后台路径是 `fork`+`execve` 还是 `posix_spawn`?**这一个就能定性 C**。
2. hang 住的时刻 `ls -l /proc/<pid>/fd` —— 那个读不到 EOF 的管道还剩几个写端。
3. 监督方判断任务存活用的是 `waitpid` 还是 pidfile + `kill(pid,0)`?**定性 pid 恒为 1**。

`setsid` 那条**不是原因**:报告者自己说调用方忽略失败,实测也确认失败不终止任何东西。
它是噪声,§3.5 处理噪声本身。

---

## 6. `getpid()` 对每个上下文都答 1 —— **已实现并实测,建议纳入本次发布**

实测:

```
parent:            getpid()=1   父亲看到 fork 子是 1001
  fork child:      getpid()=1   被起的程序是 1002
  spawned program: getpid()=1
```

后果:一个上下文**无法得知自己的身份**。写 pidfile、按 pid 命名临时文件、按 pid
打日志前缀,全部退化成同一个值。

**可修的那一半**:`fork` 的子进程。`__okm_child_record` 现在在 `kal_space_start`
**之后**分配 pid,而复制是在 `kal_space_start` 发生的,所以子进程看不到。
⇒ 改成**先占表槽拿到 pid、写进一个全局、再 `kal_space_start`**——这正是
`okm_fork.c` 已经在用的同一个手法:`g_carried_tp` / `g_carried_self`
(声明在 `:112-113`)在 `:133-134` **写于复制之前**,在 `:144-145` 由子进程读回,
而 `kal_space_start` 在 `:155`。新增一个 `g_carried_pid` 落在同一个位置即可。

**修不了的那一半**:被 `spawn` 起的**独立映像**。它是另一个程序,openkal 没有
「告诉我你给我编的号」这种操作。经环境变量塞过去是可行的,但那会污染子程序的
环境,**不推荐**。

### 6.1 实现(已写出来跑过,在 `scratchpad/musl-patched/`,**未动工作树**)

三处改动,一处是新的,两处是**被它牵出来的**:

**① `okm_syscall.c` —— 把「记录」拆成「预定 / 落定 / 退还」**

`__okm_child_record` 原本在一次调用里既分配标识又存句柄;现在多一组
`__okm_child_reserve(&pid)` / `__okm_child_commit(slot, h)` / `__okm_child_release(slot)`,
`__okm_child_record` 用它们重写(对 `okm_spawn.c` 的调用者签名不变)。

⚠️ `reserve` **不取锁**,因为 `__okm_fork` 调用它时已经持有——那把锁不可重入,
而复制必须在「没有别的上下文正改到一半」的时刻取,所以本来就得在锁内。

**② `okm_fork.c` —— 标识在复制之前就存在**

```c
int reserved_pid = 0;
const int slot = __okm_child_reserve(&reserved_pid);
if (slot < 0) { okm_unlock(); return -EAGAIN; }
g_carried_pid = reserved_pid;            /* 与 g_carried_tp 同一手法、同一位置 */
...
if (setjmp(g_resume) != 0) {             /* 复制方 */
        __okm_set_tp(g_carried_tp); __okm_set_self_pid((int)g_carried_pid);
        ...
}
const int e = kal_space_start(...);
if (e != kal_ok) { __okm_child_release(slot); okm_unlock(); return -okm_errno(e); }
__okm_child_commit(slot, child);
```

⚠️ **表槽是在上下文之前拿的,所以启动失败必须退还**,否则一个每次 `fork` 都失败的
程序会把表耗尽,然后为一个与「它有几个孩子」毫无关系的理由开始报 EAGAIN。

**③ ⚠️ 被牵出来的:所有拿 `1` 当「自己」的比较都得跟着改**

这是我一开始没预见、写的时候才撞上的。`SYS_kill` 用
`if (pid == 1 || ...) return signal_self(sig);` 判断「打给自己」——
一旦复制方的标识变成 1001,**`raise` 和 `abort` 在每一个复制里都会报 ESRCH**,
上一版刚修好的 abort 会当场回退。`getpgid`/`getsid` 同理。
⇒ 三处一律换成 `g_self_pid`。

### 6.2 判据与读数(A/B 对照 + 宿主对照)

```
########## 打了补丁 ##########
the original still answers 1                    ok   getpid()=1
a fork child answers its own identifier         ok   parent was given 1001, child says 1001
kill(getpid(),0) still works in the copy        ok   -> 0
getpgid/getsid follow the copy's identifier     ok   getpgid(0)=1001 getsid(0)=1001
abort() in a copy is still SIGABRT              ok   status=0x0006 WIFSIGNALED=1 WTERMSIG=6
a nested copy also names itself                 ok   grandchild says 1004
the original is unchanged after forking         ok   getpid()=1
-- failures: 0 --

########## 未打补丁的 0.9.0(A/B 对照) ##########
a fork child answers its own identifier         FAIL parent was given 1001, child says 1
a nested copy also names itself                 FAIL grandchild says 1
-- failures: 2 --
```

⭐ **A/B 干净**:退回 `aab97bc` 只留探针,红的**正好是**这次要修的两条,
其余五条(含 `abort`、`kill(getpid(),0)`)两边都绿——说明判据卡住了改动本身,
而不是卡住了一堆无关的东西。

**回归**:现有两个探针对打了补丁的端口重跑,
`examples/subprocess --fork --shell --abort-signal` → `-- failures: 0 --`,
`examples/posix` → `-- failures: 0 --`。没有退化。

> 宿主 glibc 上这份探针有 3 条红,全部是**探针自己写死的端口特性**
> (「原件答 1」「getsid==getpid」),不是宿主的缺陷;真正有判别力的三条
> (复制方自称、嵌套、abort)宿主全绿,与打了补丁之后一致。

### 6.3 修不了的那一半,以及建议

**修不了**:被 `spawn` 起的**独立映像**。它是另一个程序,openkal 没有
「告诉我你给我编的号」这种操作。经环境变量塞过去可行,但会污染子程序的环境,
**不推荐**。⇒ 记录:一个被起的程序自报 1,而它的父亲称它为 1001+。

⇒ **建议纳入本次发布(P1)**。风险已经从「未评估」变成「已跑过」:改动集中在
`__okm_fork` 的时序和三处 `1` 的比较,判据 7 条 + 回归 2 套全绿。
⚠️ 唯一要 review 盯的是 ③ ——**它说明这个改动会牵动 `abort` 那条路**,
合入时判据里必须保留「复制里的 abort 仍是 SIGABRT」这一条。

（下面是这条最初被列为未决时写的理由,保留不改。）
⚠️ 这条**只有读码和实测,没有做过改动**,风险未评估(改的是 fork 的时序),
**请 review 决定是否纳入本次发布**,还是单独一轮。

---

## 7. 建议的落地顺序

| 批次 | 内容 | 理由 |
| --- | --- | --- |
| **P0** | A、B、F1、F2、F3 | 全是「报了成功而没做」或「把错误当结果返回」。F1/F2 是数据完整性级别 |
| **P1** | D、E、F7、F8、**§6 `getpid`** | 答得出却没答,或答得不诚实。改动都很小;`getpid` 已实现并跑过判据与回归 |
| **P2** | 文档:C、F5、F6、F4、F9-F13 全部进分歧表;改掉 `execve` 那句错话 | 不改行为,但**没有它这次发布是在重复上一轮的错误**——上一轮的分歧表漏了正是这些 |
| **P3** | 上报:openkal(**`kal_fs_lock` + `KAL_FS_PROP_LOCKS`**、寿命受限的起法、目录时间、处理器数量、卷容量、fd 位置一般化)、openkal-linux(CLOEXEC 回报管道) | 不阻塞发布。⭐ 锁这一条**优先级最高**:它是唯一一条「三个环境都做得到、只差一个词」的 |
| 待定 | §3.6:F1 接受 ENOSYS(推荐),还是暂时只修 F2 | 请 review 拍板 |

---

## 8. 明确不做

- **不**改 `chmod`/`fchmodat`。上一轮理由成立,报告者也接受。
- **不**在端口里模拟进程组与会话。§3.5 改的只是**答案的措辞**,不制造不存在的效果。
- **不**在端口里模拟文件锁——**但也不把它当成 `chmod` 那样的永久拒绝**:
  §3.6 说明了 sidecar 方案为什么会把「持锁时崩溃」变成永久锁死,以及为什么这一条
  该向规范要原子而不是在 libc 里造。
- **不**在本仓修 C 的根本,那需要 openkal 的新原子(§3.3.0 说明了它是规范缺口
  而不是后端缺陷)。
- **不**动 `okm_resolve` 让它做存在性检查——它是词法解析器,存在性属于
  `kal_fs_info`;混进去会让每一次 `open` 都多一次往返。

---

## 9. 复现材料

本轮新写七个探针,全部对 **openkal-musl 0.9.0 + openkal-linux 0.7.1** 编译运行,
**每个都有宿主 glibc 对照**:

| 探针 | 证明 |
| --- | --- |
| `execprobe` | A(逐字复现报告者五行)+ B |
| `orphan` | C(marker 判定 + `fork`/`spawn` 两路对照) |
| `fsprobe` | D + E |
| `kalprobe` | D 的关键一步:身下**做得到** |
| **`surface`** | **26 项宽面对照,13 项不一致** —— F1、F3、F4、F5、F6、F7、F8、F9-F13 都出自它 |
| **`lockprobe`** | **F1、F2**,两个进程 + 宿主对照 |
| **`idprobe`** | F3,以及 §6 的 `getpid` 恒为 1 |
| **`xprobe`** | **F5、F7**,两条都要「装了再查」才看得出,单看返回值全是 0 |
| **`pidfix`** | §6 的实现验证:打了补丁的端口副本 + A/B 对照 + 回归 |

⭐ §6 的补丁在 `scratchpad/musl-patched/`(`port/src/okm_syscall.c`、
`port/src/okm_fork.c` 两个文件),**工作树未动**。合入时直接取这两个文件的差异即可。

⭐ 落地时 `surface` 应当**整个搬进 `examples/`**,而不只是搬那 13 条:
它的价值在于**下一族缺陷会先撞上它**,而不在于它这次命中了什么。
这正是上一轮 `examples/subprocess` 七条全绿却漏掉整族的反面。
