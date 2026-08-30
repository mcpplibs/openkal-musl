# openkal-linux#13 第五轮反馈:PATH 搜索,以及它盖住的三条

2026-08-30 · 源码核查 + **实测复现** + 设计方案 · 待 review,尚未实施

核实基线 —— **与报告者 `yspbwx2010` 完全同版本**:openkal-musl **0.9.0**
(`aab97bc`)、openkal-linux **0.7.1**、openkal-llvm-runtime 0.5.0,
目标 `x86_64-linux-musl`。

> ⚠️ 写这份文档时本地检出停在 0.7.0(`250f002`),比 `origin/main` 落后两个提交。
> 已 `git fetch` 并核对 0.7.0→0.9.0 的差异:`port/src/okm_syscall.c` 只增加了版本
> 横幅、`SYS_truncate` 和 `uname` 的 release 字段,`port/src/okm_spawn.c` 与
> `musl/PATCHES.md` **一字未改**。本文所有行号均取自 **0.9.0**,所有读数均取自
> **对 0.9.0 的实际运行**,不是对 0.7.0 的推断。

标「读码」的附文件行号;标「实测」的附程序、命令与读数,且每条都带**宿主 glibc
对照**——没有对照的读数只能说明程序做了什么,不能说明它做错了什么。

---

## 0. 结论

**报告者问的那一条是 openkal-musl 侧的缺陷,不是使用侧的问题。** 已完整复现,
定位到行,机制清楚。

顺着同一处读码和实测,又挖出**两条报告里没有、且更严重**的缺陷(B、C),其中
C 是这个端口自己的规矩最不能接受的形状——**读数正确而事实相反**。

另外报告者对 EISDIR 那条的**归因是错的**(§4),但那一条本身确实是我们的缺陷。

| # | 条目 | 归属 | 状态 |
| --- | --- | --- | --- |
| **A** | `execvp` 过不了 PATH 的第一次未命中 | **openkal-musl**(+ openkal-linux 分担) | 实测复现,§1 |
| **B** | `posix_spawnp` **根本不搜 PATH**,还报成功 | **openkal-musl** | 报告里没有,实测,§2 |
| **C** | `kill()` 打不到 `fork`+`execve` 起的程序 | **openkal-musl**(今天无法在本仓修完) | 报告里没有,实测,§3 |
| **D** | 目录 mtime 报 EISDIR | **openkal-musl**,但**报告者归因错了** | 实测,§4 |
| **E** | `setsid`/`setpgid` 报 ENOSYS | openkal-musl,自相矛盾 | §5 |
| F | 权限位 7 条 | **使用侧**(双方已达成一致) | §6 |
| G | `compile_commands.json` 串版本 | **使用侧**(报告者自己已定位) | §6 |
| H | 后台任务不终结 / 一处 hang | 待定 —— **但 C 是新的首选假设** | §7 |

**一句话**:报告者说「这一条 libc 路径解释了十九条里的九条」。读完之后要改成——
这一条路径解释了九条,而**盖在它下面的 C 很可能解释了剩下那两条它自己承认没定位的**。

---

## 1. 缺陷 A:`execve` 起不来程序时不返回,而是把调用者结束掉

### 1.1 实测(先于读码,因为这条能直接复现)

报告者给了完整程序。原样编译运行,**五行读数逐字一致**,并补了两行
`posix_spawnp`(§2):

```
$ OPENKAL_MUSL_TRACE=enosys ./execprobe
openkal-musl 0.9.0
execvp "/bin/sh"                     child said hi              exit=7
execvp "sh"                          child said (nothing)       exit=127
execvp "sh"  PATH=/usr/bin           child said hi              exit=7
execvp "sh"  PATH=/nope:/usr/bin     child said (nothing)       exit=127
execvp "sh"  PATH=/usr/bin:/nope     child said hi              exit=7
```

⭐ **`OPENKAL_MUSL_TRACE=enosys` 一行都没打。** 这条不是缺失的系统调用,所以
上一轮加的那个诊断通道看不见它——这一点本身值得记下来(§8)。

### 1.2 读码 —— 三环,每一环可复核

1. `port/src/okm_syscall.c:1601-1611` —— `SYS_execve` 表达为
   「起一个程序、等它、拿它的状态结束自己」:

   ```c
   case SYS_execve: {
       pid_t child = 0;
       const int e = __posix_spawn(&child, (const char*)a1, 0, 0, ...);
       if (e) return -e;                                    /* ← 只有这一条路返回 */
       int st = 0;
       if (do_wait4((int)child, &st, 0, 0) < 0) kal_exit(127);
       kal_exit((st & 0x7f) ? 128 + (st & 0x7f) : ((st >> 8) & 0xff));
   }
   ```

   `if (e) return -e` 是**在的**——所以只要 `__posix_spawn` 会报错,`execve` 就会
   带 `errno` 返回。问题是它不报。

2. `port/src/okm_spawn.c:185` —— `okm_resolve` **纯粹是词法的**,不做任何存在性
   检查(`okm_fd.c:405-467`:拼接、规范化、挑最长前缀的 preopen)。它只在名字落在
   所有 preopen 之外时才返回 `-ENOENT`。`/nope/sh` 落在 `/` 这个 preopen 里面,
   所以顺利通过。

3. **`openkal-linux/src/process.cpp:81-101`**(`kal_process_spawn_with` 在
   `:190-212` 是同一份)—— 真因在这里:

   ```cpp
   const okl_long child = okl::sys(okl::nr_clone, 17 /* SIGCHLD */, 0, 0, 0, 0);
   if (okl::failed(child)) return okl::translate(child);
   if (child == 0) {
       ...
       okl::sys(okl::nr_execveat, b, ..., 0);
       okl::sys(okl::nr_exit_group, 127);      /* ← exec 失败,子进程自己退 127 */
       for (;;) { }
   }
   *out = kal_process{ ... };
   return kal_ok;                              /* ← 父亲拿到 kal_ok */
   ```

   **`kal_process_spawn` 没有回报管道。** exec 失败只有子进程知道,父亲拿到的是
   `kal_ok` 和一个句柄。

⇒ 于是:`__posix_spawn` 返回 0 → `SYS_execve` 走到 `do_wait4` → 状态是 127 →
`kal_exit(127)`。**调用者被结束掉了,`execve` 从来没返回过。**

### 1.3 为什么 musl 的 `execvp` 必然过不去

`musl/src/process/execvp.c:38-47`:

```c
execve(b, argv, envp);
switch (errno) {
case EACCES: seen_eacces = 1;
case ENOENT:
case ENOTDIR:  break;        /* 继续试下一个 PATH 项 */
default:       return -1;
}
```

这个循环**完全建立在「`execve` 失败会返回」之上**。这里它不返回,所以第一次未命中
就是终点。名字里带 `/` 直接走 `execve`,这就是第一行为什么过。

### 1.4 ⚠️ 两份文档现在说的话是错的,必须改

`README.md:315` 和 `musl/PATCHES.md:114` 都写着 `execve` 这个表达
**「A caller cannot distinguish that through this library」**。

报告者把这句原话引了出来并且指出:**exec 成功时它成立,失败时不成立**——失败时
没有程序可起、没有状态可终,这个安排必须以某种方式作答,而它选择了「以 127 结束
调用者」。

他是对的。而且比他说的更严重:§3 证明**即使 exec 成功,调用者也能分辨**。

### 1.5 设计

#### A1 —— 起之前先问名字在不在(`okm_spawn.c`,本仓)

在 `okm_resolve` 之后、`okm_process_spawn` 之前,加一次 `kal_fs_info`:

```c
struct kal_node_info info = { .self_size = sizeof info };
const int ie = okm_fs_info(at.base, at.rel, slen(at.rel),
                           0 /* 解析,与 open 一致 */, KAL_INFO_KIND, &info);
if (ie != kal_ok)                     refused = okm_errno(ie);
else if (info.kind == kal_node_absent)    refused = ENOENT;
else if (info.kind == kal_node_directory) refused = EACCES;  /* POSIX:目录是 EACCES */
```

三个约束,都不能漏:

- **必须走 `refused` 那条既有的收尾路径**(`okm_spawn.c:314-318`),否则 file
  actions 开的文件泄漏、锁不释放。
- **必须在 `_WIN32` 的 `.exe` 重试之前想清楚**(`okm_spawn.c:342-357`):那里靠
  `kal_err_not_found` 决定要不要补后缀。前置检查会把「无后缀名字不存在」变成提前
  拒绝,`.exe` 那条路就再也走不到。⇒ 检查要写成一个 `startable()` 小函数,`_WIN32`
  分支复用它,而不是在主路径上直接 `return`。
- 这是**放在 `__posix_spawn` 里而不是 `SYS_execve` 里**。放这里同时修好 §2,而且
  让 `posix_spawn` 也同步报 ENOENT——glibc 与 musl 自己的实现都是同步报的。

#### A2 —— 剩下的洞:存在但起不来

名字在、但**不可执行**或不是有效映像,前置检查放行,后端子进程照样 `exit_group(127)`,
`execve` 照样 `kal_exit(127)`。**openkal 没有可执行位**(`kal_node_info` 只有
`writable`),所以本仓无法回答 EACCES / ENOEXEC。

⇒ **A1 修好的是 ENOENT/ENOTDIR,也就是 PATH 搜索真正需要的那两个**,报告者那五行
全部转正。剩下的洞要后端补,见 A3,并且**这个残留必须写进分歧表**,不能让 A1 看起来
像是修完了。

#### A3 —— 完整修法在 openkal-linux(另开 issue)

`kal_process_spawn` / `kal_process_spawn_with` 加一条 `O_CLOEXEC` 回报管道:
子进程 `execveat` 失败就把 `errno` 写进去;父亲 `read` 到内容就收尸并
`return okl::translate(...)`。exec 成功时管道被 CLOEXEC 关掉,父亲读到 EOF。

这是标准做法,同时**消掉 A1 的 TOCTOU**,并且把 EACCES/ENOEXEC 补齐。

> A1 仍然要做,不因为 A3 而省:openkal 允许实现拒绝接口,端口不能假设身下的后端
> 一定会同步报错。A1 是本仓对任何后端都成立的那一半。

---

## 2. 缺陷 B(报告里没有):`posix_spawnp` 根本不搜 PATH,而且报成功

### 2.1 实测

```
posix_spawnp "sh"        posix_spawnp ok, exit=127     ← 报成功,子进程 127
posix_spawnp "/bin/sh"   posix_spawnp ok, exit=7
```

宿主 glibc 上第一行是 `exit=7`。

### 2.2 读码

`musl/src/process/posix_spawnp.c` 不含循环,它把 PATH 搜索**托给子进程**:

```c
spawnp_attr.__fn = (void *)__execvpe;
return posix_spawn(res, file, fa, &spawnp_attr, argv, envp);
```

musl 自己的 `posix_spawn.c:152` 在子进程里读这个字段:
`attr->__fn ? (int (*)())attr->__fn : execve`。

⚠️ **本端口替换掉了 `posix_spawn.c`,而 `okm_spawn.c:180-181` 只看 `__flags`:**

```c
if (attr && (attr->__flags & ~(POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK)))
    return ENOSYS;
```

`__fn` **从头到尾没有被读过**。于是 `posix_spawnp("sh", …)` 把 `sh` 当相对路径,
对着工作目录起——起不来,而按 §1 的链条,`__posix_spawn` 还返回 0。

⇒ 这正是这个端口自己反复声明要避免的形状:**接受了一个请求,没有执行,报了成功**。
和上一轮 `addclose` 被判为缺陷的形状一模一样。

### 2.3 设计

- **B1(推荐)**:仿照 `okm_spawn.c` 替换 `posix_spawn.c` 的先例,再替换一份
  `posix_spawnp`,循环在**这一侧**做:名字含 `/` 直接调 `__posix_spawn`;否则按
  PATH(缺省 `"/usr/local/bin:/bin:/usr/bin"`,与 musl 一致)逐项拼接调用,
  `ENOENT`/`ENOTDIR` 继续,`EACCES` 记住,其余立即返回。
  依赖 A1 —— 没有 A1,`__posix_spawn` 不报 ENOENT,循环第一项就停。
- **B2**:在 `__posix_spawn` 内部识别 `__fn`。不推荐:会让「起这个名字」和
  「搜这个名字」两件事挤在一个函数里。
- 无论哪种,`__posix_spawn` 都应当**拒绝**(`ENOSYS`)一个它不认识的 `__fn`,
  而不是像现在这样静默忽略。

---

## 3. 缺陷 C(报告里没有):`kill()` 打不到 `fork`+`execve` 起的程序

**这条是本轮最严重的一条**,因为它的读数是对的而事实是反的。

### 3.1 实测 —— 带宿主对照,两边状态字完全相同

程序:`fork` → 子进程 `execve("/bin/sh", "-c", "sleep 3; echo X > marker.txt")`;
父进程 400ms 后 `kill(pid, SIGTERM)`、`waitpid`,再等 5 秒看 marker 在不在。

```
=== openkal-musl 0.9.0 ===
kill(1001, SIGTERM) = 0
waitpid = 1001  raw status = 0x000f  WIFSIGNALED=1 WTERMSIG=15
marker.txt EXISTS: REACHED-THE-END      <-- 被 kill 的程序跑完了全程

=== 宿主 glibc 对照 ===
kill(1194981, SIGTERM) = 0
waitpid = 1194981  raw status = 0x000f  WIFSIGNALED=1 WTERMSIG=15
marker.txt absent                        <-- SIGTERM 打到了程序
```

⚠️ **状态字一模一样(`0x000f`),两边都告诉调用者「它死于 SIGTERM」。**
一边是真的,一边是假的,而调用者手上没有任何东西能把两者分开。

范围是精确的——只有 `fork`+`execve` 这条路:

```
fork + execve   status=0x000f  program SURVIVED the kill  <-- 成了孤儿
posix_spawn     status=0x000f  program was killed
```

### 3.2 机制

`fork` composed 在 `openkal.space` 之上,复制出的 space 是一个真进程;它调
`execve`,按 §1 那条链**又起了一个真进程**并停在 `do_wait4` 上等。所以进程有三层:

```
调用者  ──起──▶  fork 出来的 space(只负责等)  ──起──▶  真正的 sh
```

父亲的 `g_child` 表(`okm_syscall.c:528`)记的是**中间那个等待者**。
`SYS_kill`(`okm_syscall.c:1615` 一带)对它调 `okm_process_terminate` ——
等待者死了,**`sh` 毫发无伤**,继续跑到底。

而父亲收到的是「1001 死于 SIGTERM」,因为等待者确实死于 SIGTERM。

### 3.3 ⚠️ 今天在本仓修不完,这一点要说清楚

`kill` 发生在父亲这一侧,父亲**无法知道**自己的哪个孩子是等待者;等待者阻塞在
`kal_process_wait` 里,**收不到任何东西也跑不了代码**。openkal 今天没有任何原子
能表达「这个程序的寿命以我为界」。

⇒ 分三步,前两步是本仓的:

- **C1(必做,先做)**:`README.md:315` 与 `musl/PATCHES.md:114` 那句
  「a caller cannot distinguish」**是错的,要改掉**,并在分歧表里如实写明:
  经 `fork`+`execve` 起的程序,`kill` 只到达中间映像,状态字仍报信号死亡。
  ⚠️ 这一句现在读起来像是「已经想清楚且无代价」,而它正是这条缺陷藏身的地方。
- **C2(必做)**:补一条判据把当前行为钉住(§8),否则改好了也没人知道。
- **C3(上报)**:向 openkal 要一个「寿命受调用者约束」的起法(Linux 侧是
  `PR_SET_PDEATHSIG`,Windows 侧是 job object),`execve` 用它起替身。
  这是唯一能真正修好的路。

  > 顺带排除一个看起来可行的方案:让 openkal-linux 的
  > `kal_process_terminate` 杀进程组。要让它成立,`kal_process_spawn` 就不能给
  > 每个被起的程序开新组;可一旦不开新组,「终止 A」就会连带杀掉 A 起的所有程序,
  > 而 POSIX 下它们本该活着。**换了一个错,不是修好。**

### 3.4 给使用侧的当下规避

在修好之前,**需要能 `kill` 的地方用 `posix_spawn` / `system` / `popen`,
不要用 `fork`+`execve`**——实测那三条路的 `kill` 是到位的。

---

## 4. 缺陷 D:目录 mtime —— 是缺陷,但**报告者归因反了**

报告者写的是「Reading a directory's mtime fails with EISDIR」,并解释说读目录 mtime
是他们锁协议的正常用法而不是笔误。

### 4.1 实测:读是**好的**,坏的是**写**

```
stat(dir)          -> 0  errno=0   (GETTER 路径,正常)
utimensat(file)    -> 0  errno=0
utimensat(dir)     -> -1 errno=21  Is a directory     (SETTER 路径)
```

> 附带更正一处我自己的读数:第一版探针里我把 `stat(d,&st)` 写在 `printf` 的实参
> 里,求值次序未定义,于是打出一个荒唐的 mtime。改正后目录 mtime 与宿主逐位一致
> ——**没有 mtime 缺陷**。

### 4.2 为什么两者的报错文字一样

libc++ 的两个重载**用同一个名字报错**
(`llvm/libcxx/src/filesystem/operations.cpp:679` 与 `:691`,都是
`ErrorHandler<…> err("last_write_time", …)`):

- 取值 → `posix_stat` → `::stat` → 本端口 `do_fstatat`,目录正常;
- 赋值 → `set_file_times` → `time_utils.h:316` 的 `::utimensat(AT_FDCWD, …)`。

⇒ `filesystem error: in last_write_time: Is a directory` **只可能来自赋值**。
请报告者确认那个调用点:多半是**续锁 / 打时间戳**,不是判过期时的读。

### 4.3 读码 —— 我们这侧确实错了

`okm_syscall.c:1116` 对具名路径**无条件**这样开:

```c
int e = okm_fs_open(at.base, at.rel, slen(at.rel),
                    KAL_OPEN_READ | KAL_OPEN_WRITE, &f);
```

`openkal-linux/src/fs.cpp` 把 `READ|WRITE` 映射成 `O_RDWR`,对目录 → `EISDIR`
→ `kal_err_is_directory`(`sys.h:278`)→ `okm_fd.c:58` → `EISDIR`。

### 4.4 ⭐ 实测:这件事身下**做得到**,只是我们要错了权限

直接调 openkal 层:

```
kal_fs_open(dir, READ|WRITE) -> 12 (拒绝, kal_err_is_directory)
kal_fs_open(dir, READ)       -> 0  (ok)
  kal_fs_set_modified(dir)   -> 0  (ok)
  stat after                 -> mtime=1700000000 (正是要设的值)
```

**目录的时间被真的改掉了。** 所以这不是「身下做不到」。

⚠️ 但它**在规范说的话之外**:`fs.h:273` 明写
「The file shall have been opened with KAL_OPEN_WRITE」,而 `kal_fs_open` 说的是
「Opening a file」,目录归 `kal_fs_open_dir`(产出 `kal_dir`),而
`kal_fs_set_modified` **没有收 `kal_dir` 的形式**。

### 4.5 设计 —— 两条路,请 review 定夺

- **D1(我推荐)**:先 `kal_fs_info` 问种类;是目录就退到 `KAL_OPEN_READ` 再
  `kal_fs_set_modified`;失败就如实上报。
  - 理由:这**不是模拟,也不是静默的错答案**——调用者要的效果实实在在发生了,
    做不到的实现会返回错误而我们照实翻译。和 `chmod` 的情形**不同**:`chmod` 被拒
    是因为映射过去会「报成功而做了别的事」,这里不会。
  - 代价:踩在规范的前置条件之外,**必须记进分歧表**,并**上报规范**(要一个目录
    形式,或把 `kal_fs_set_modified` 改述在名字上)。openkal-windows 很可能做不到,
    那里会如实失败。
- **D2(纯粹派)**:报 `ENOSYS` 而不是 `EISDIR`,和 `chmod`/`symlink` 并列进分歧表,
  等规范。
  - `ENOSYS` 至少比 `EISDIR` 诚实:`EISDIR` 是在说「你传错了类型」,调用者会去查
    自己的代码;`ENOSYS` 是在说「这个环境没有这个操作」。
  - 代价:一个每个身下环境都做得到的普通 POSIX 操作就此长期不可用。

⚠️ 无论选哪条,`musl/PATCHES.md:125` 那段都要补:它今天只记了「要写权限而不是
要所有权」,**没有记「目录的时间根本设不了」**。

---

## 5. 缺陷 E:`setsid`/`setpgid` 报 ENOSYS,与本端口自己的回答矛盾

### 5.1 实测

```
setsid()      -> -1 errno=38 (ENOSYS)
setpgid(0,0)  -> -1 errno=38 (ENOSYS)
getpid=1  getpgid(0)=1  getsid(0)=1
```

### 5.2 矛盾在哪里

`okm_syscall.c:1857-1876` 的注释已经想清楚了一半:没有组也没有会话,所以
`getpgid`/`getsid` 诚实地回答「就一个程序,它自成一组」。然后接着说
「`setpgid` 与 `setsid` 仍然拒绝:造一个组和身处一个组不是一回事」。

⚠️ **但这两个调用问的恰恰不是「造一个组」:**

- `setpgid(0, 0)` 请求的状态是「调用者自成一组」——按上面那三行读数,
  **这个状态已经成立**。它不是要求一个不存在的效果,它要求的是已经为真的事。
  ⇒ 应当**返回 0**。这不是报告一个不存在的效果,而是报告一个已经存在的效果。
- `setsid()` 在 POSIX 下有一个成文的失败:**调用者已经是进程组组长时返回 EPERM**。
  而 `getpgid(0) == getpid()` 正是「已经是组长」的断言。
  ⇒ 应当返回 **EPERM**,这是真话,不是搪塞。
- `setpgid` 指名其它任何东西 → `EPERM`(这个环境没有能命名的第二个组)。

**为什么这不只是好看**:守护化代码普遍处理 `EPERM`(`fork`-然后-`setsid` 这套
舞步就是为它存在的),**没有一份处理 `ENOSYS`**。一个成文的失败调用者接得住,
一个陌生的失败接不住。

顺带:这会消掉报告者 trace 里 16 行(`setpgid` 12 + `setsid` 4)。

> ⚠️ 这条是**判断**而不是读码结论,和 §1-§4 不同级别,单独列出来等 review 否决。

---

## 6. 使用侧的部分

- **权限位 7 条**(目录/文件的限制性 mode、解包时的可执行位、写后 chmod):
  **使用侧**。双方在前几轮已就理由达成一致,报告者也接受。
  端口这侧**无事可做**:上一轮认领的那件事(`open`/`mkdir` 的 `mode` 被静默丢弃)
  已经落地,`README.md:115` 单独成行,并在 `:122` 给出了替代写法。复核确认。
- **`compile_commands.json` 串版本**:**使用侧**,报告者自己定位并给出了修法。
  不需要我们做任何事。
- **`bwrap` 探测拿到 127 判定「未安装」**:归 **A**,不是使用侧。
  A 修好之后自然消失。

---

## 7. 仍然开放的两条 —— 但首选假设变了

上一轮我们说这两条「从源码到不了」。现在有 C 了,**它对两条都是自洽的解释**:

**「后台任务永不终结」。** 报告者说子进程确实在跑、日志确实在写,只是父亲观察不到
它结束。如果这个任务是 `fork`+`execve` 起的,而监督方在超时时 `kill` 它:按 §3,
等待者死掉、父亲拿到一个信号死亡的读数,**真正的程序还在跑并继续持有它的输出端**。

**那处 hang(无子进程、无缺失系统调用、60 秒无输出)** 是同一个机制的下一步:
父亲读管道等 EOF,而管道的写端在那个**没被杀掉的孤儿**手里,EOF 永远不来。
「无子进程」正好吻合——从父亲的表看它确实已经没有孩子了。

⇒ 需要三个观察,每个都能证伪:

1. 那条后台路径是 `fork`+`execve`,还是 `posix_spawn`/`system`?**这一个就能定性。**
2. hang 住的时刻,`ps` 里有没有一个仍然活着的目标程序(它的父亲会是 1 或已消失)?
3. hang 住的时刻,`ls -l /proc/<pid>/fd` —— 那个读不到 EOF 的管道还剩几个写端。

`setsid` 那条**不是原因**:报告者自己说调用方忽略失败,而实测也证实
`setsid` 返回失败不会终止任何东西。它是噪声,§5 处理的是噪声本身。

---

## 8. 判据(与修复同批,缺一条都不算修完)

现有探针 `examples/subprocess` **正是漏掉这一整族的那个**——它从不按裸名字起程序,
也从不 `kill` 一个 `fork`+`execve` 起的程序。要补的:

| # | 判据 | 对应 |
| --- | --- | --- |
| 1 | `execvp("sh")` 在 `PATH=/nope:<真目录>` 下起得来 | A |
| 2 | `execve("/不存在")` **返回 -1 且 `errno==ENOENT`**,调用者活着 | A |
| 3 | `execve("<一个目录>")` 返回 -1 且 `errno==EACCES` | A1 |
| 4 | `posix_spawnp("sh", …)` 起得来;`posix_spawnp("/不存在")` 返回 ENOENT | B |
| 5 | ⭐ `fork`+`execve` 起的程序被 `kill` 后**确实停了**(用它写不出的 marker 判) | C |
| 6 | 目录的 `last_write_time` 设得上(或按 D2 报 ENOSYS,二选一钉死) | D |
| 7 | `setpgid(0,0)==0`、`setsid()==-1 && errno==EPERM` | E |
| 8 | **控制项**:上述每一条都在宿主目标上跑同一份源码并给出同样读数 | 全部 |

⚠️ **A/B 对照是必须的**:把 `port/src` 退回 `aab97bc` 只留新探针,1-7 必须**红**。
一条在缺陷上就是绿的判据,证明不了任何事——上一轮的自我 review 已经在这上面栽过
一次。

⭐ 另外记一笔:**这一族缺陷 `OPENKAL_MUSL_TRACE=enosys` 一条都看不见**,因为它们
不是缺失的操作,而是**在场却答错的操作**。上一轮把诊断通道当成「下一轮更便宜」的
答案,这一轮证明它只覆盖了一半。是否要一个「起程序失败」的 trace 位,留待 review。

---

## 9. 文档要改的地方

| 位置 | 改什么 |
| --- | --- |
| `README.md:315` | 删掉/限定「a caller cannot distinguish」——A 与 C 都是反例 |
| `musl/PATCHES.md:114` | 同上,并写明 `kill` 只到达中间映像 |
| `musl/PATCHES.md:125` | 补「目录的修改时间」这一情形(按 D1 或 D2 的结论写) |
| `README.md` 分歧表 | 补 A2 的残留(名字在但起不来 → 仍以 127 结束调用者) |

(`open`/`mkdir` 的 `mode` 被静默丢弃**已经记了**,`README.md:115`,本轮无需再动。)

---

## 10. 明确不做

- **不**把 `chmod`/`fchmodat` 改成可用。上一轮的理由成立,报告者也接受。
- **不**在 openkal-musl 里模拟进程组与会话。§5 改的只是**答案的措辞**,
  不制造任何不存在的效果。
- **不**在本仓修 C 的根本(§3.3),那需要 openkal 的一个新原子;本轮只做记录 + 判据
  + 上报。
- **不**动 `okm_resolve` 让它做存在性检查。它是词法解析器,存在性属于 `kal_fs_info`;
  混进去会让每一次 `open` 都多一次往返。

---

## 11. 复现材料

四个探针,全部为本轮新写,均对 **openkal-musl 0.9.0 + openkal-linux 0.7.1** 编译运行,
且每个都有宿主 glibc 对照:

| 探针 | 证明 |
| --- | --- |
| `execprobe` | A(逐字复现报告者五行)+ B(两行 `posix_spawnp`) |
| `orphan` | C(marker 判定 + `fork`/`spawn` 两路对照) |
| `fsprobe` | D(`stat` 对目录好、`utimensat` 对目录坏)+ E |
| `kalprobe` | D 的关键一步:`kal_fs_open(dir, READ)` + `kal_fs_set_modified` **做得到** |

`execprobe` 的源码在 issue 里(报告者给的原文,我只加了两行 `posix_spawnp`)。
另外两个是本轮新写、证明 C 与 D 的关键材料,全文附在下面。落地实施时应搬进
`examples/`,与 §8 的判据合并。

### 11.1 `orphan` —— 证明 C

判据的形状是关键:**用一个「被杀掉就写不出来」的 marker 判定**,而不是看状态字
——状态字两边相同,正是它骗过了所有人。

```c
#define _GNU_SOURCE
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
extern char** environ;

static void nap_ms(long ms){ struct timespec t={ms/1000,(ms%1000)*1000000}; nanosleep(&t,NULL); }

static void probe(const char* how, int use_spawn) {
    unlink("marker.txt");
    pid_t pid = -1;
    char* av[] = { "/bin/sh", "-c", "sleep 3; echo X > marker.txt", NULL };
    char* ev[] = { NULL };
    if (use_spawn) {
        if (posix_spawn(&pid, "/bin/sh", NULL, NULL, av, ev) != 0) { printf("%s: spawn failed\n", how); return; }
    } else {
        pid = fork();
        if (pid == 0) { execve("/bin/sh", av, ev); _exit(66); }
    }
    nap_ms(400);
    kill(pid, SIGTERM);
    int st = 0; waitpid(pid, &st, 0);
    nap_ms(5000);                       /* 越过那个程序自己的延时 */
    FILE* f = fopen("marker.txt", "r");
    printf("%-24s status=0x%04x  program %s\n", how, st,
           f ? "SURVIVED the kill  <-- orphaned" : "was killed");
    if (f) fclose(f);
    unlink("marker.txt");
}

int main(void) { probe("fork + execve", 0); probe("posix_spawn", 1); return 0; }
```

### 11.2 `kalprobe` —— 证明 D 身下做得到

绕过 musl 直接调 openkal,分开「`READ|WRITE` 被拒」与「`READ` 可以且时间真的改了」。

```c
#include <openkal/fs.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    mkdir("kal.dir", 0700);
    struct kal_dir base; char nm[256]; kal_uintptr nl = 0;
    if (kal_fs_preopen(0, &base, nm, sizeof nm - 1, &nl) != kal_ok) return 1;
    nm[nl] = 0;
    printf("props MODIFIED_TIME = %d\n",
           (int)((kal_fs_props(base) & KAL_FS_PROP_MODIFIED_TIME) != 0));

    /* 名字必须相对于 preopen */
    char rel[512], cwd[512];
    getcwd(cwd, sizeof cwd);
    const char* r = cwd + nl; while (*r == '/') r++;
    if (*r) snprintf(rel, sizeof rel, "%s/kal.dir", r);
    else    snprintf(rel, sizeof rel, "kal.dir");

    struct kal_file f;
    int e = kal_fs_open(base, rel, strlen(rel), KAL_OPEN_READ | KAL_OPEN_WRITE, &f);
    printf("kal_fs_open(dir, READ|WRITE) -> %d\n", e);          /* 12 = is_directory */
    if (e == kal_ok) kal_fs_close_file(f);

    e = kal_fs_open(base, rel, strlen(rel), KAL_OPEN_READ, &f);
    printf("kal_fs_open(dir, READ)       -> %d\n", e);          /* 0 */
    if (e == kal_ok) {
        printf("  kal_fs_set_modified(dir)   -> %d\n",
               kal_fs_set_modified(f, (kal_u64)1700000000ull * 1000000000ull));
        kal_fs_close_file(f);
        struct stat st; stat("kal.dir", &st);
        printf("  stat after                 -> mtime=%ld\n", (long)st.st_mtime);
    }
    rmdir("kal.dir");
    return 0;
}
```

三者的 `mcpp.toml` 都是同一份,**用发布版而不是 path 依赖**,以保证读的是报告者
跑的那个:

```toml
[dependencies]
openkal-musl = "0.9.0"

[build]
cxx_runtime = "host-coupled"    # kalprobe 不需要这一行以外的任何东西
```
