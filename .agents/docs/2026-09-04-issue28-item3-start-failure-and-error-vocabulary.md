# issue #28 第 3 条:反馈核实、生态适配范围与方案

2026-09-04 · 待 review,**一行代码都还没写**
来源:<https://github.com/mcpplibs/openkal/issues/28#issuecomment-5540511408>(yspbwx2010)
前置:`2026-08-31-four-remaining-and-what-they-are-really.md`、
`2026-08-30-openkal-0.10-ecosystem-plan.md`(§0 的「整张图必须整体移动」是本轮的成本依据)

---

## 0. 一句话总览

**第一个结论:反馈成立,但它测到的不是最严重的那一条。**

顺着它去核实四个实现时发现:**`openkal-macos` 的 `kal_process_spawn` 对任何起不来的
名字都返回 `kal_ok` 加一个句柄 —— 包括一个根本不存在的名字。** 这正是 clause 3.1
用来区分「供给」与「模拟」的那条线,而且它**可以被 conformance 可移植地测到**,
不需要文件系统、不需要可执行位、不需要新的错误值。反馈者看不到它,是因为
`openkal-musl` 的 `startable()` 先问了 `kal_fs_info`,替 macOS 挡住了。

| # | 事项 | 判据 | 动规范吗 | 落点 |
|---|---|---|---|---|
| 1 | openkal-macos:起不来的名字返回 `kal_ok` | **M1** clause 3.1 | no | openkal-macos |
| 2 | `kal_error` 缺「不是这个环境能启动的形式」 | **M1** clause 3.1 + 5.2 先例 | **要** | openkal + 3 实现 + musl |
| 3 | 上面两条今天没有任何检查能发现 | **M3** clause 9.1 | no | openkal/conformance + musl probe |

**只有第 2 条动规范。** 第 1、3 条是实现与套件内部。

⇒ 另外核出一条**真实存在、但本轮明确不做**的缺陷(网络错误值三个实现不一致),
理由在 §6.1 —— 它的每一种可选修法都是「把一个错答案换成另一个错答案」。

---

## 1. 反馈核实

### 1.1 逐条对着源码核,全部成立

| 反馈的主张 | 核实位置 | 结论 |
|---|---|---|
| `translate` 无 ENOEXEC 分支,落 `default` → `kal_err_io` | `openkal-linux/src/sys.h:304-322` | yes |
| ENOEXEC 在该文件里根本没定义 | 同上 `:210-216` 的 errno 常量表 | yes |
| 报告通道本身是通的 | `openkal-linux/src/process.cpp:319` `report.say(why)` → `:324` `report.heard()` | yes |
| `okm_errno` 的 `default: EIO` 轮不到 | `openkal-musl/port/src/okm_fd.c:43-62` | yes |
| `kal_error` 今天十三个值 | `openkal/include/openkal/types.h` | yes |
| 644 的那个 case 完好到达 | `e_acces→6`,`kal_err_permission→EACCES` | yes |
| 端口的前置检查救不了这一条 | `okm_spawn.c` 的 `startable()` 只问 `KAL_INFO_KIND` | yes |
| 这条路径上没有 127 | `process.cpp:324-327` 先 `reap` 再 `return translate(why)` | yes |

### 1.2 我另做的独立控制读数

在本机(glibc 2.44,x86_64)对一个 755、非 ELF、无 `#!` 的文件:

```
glibc posix_spawn  -> 8 (Exec format error)
glibc posix_spawnp -> 8 (Exec format error)
raw execveat       -> ret -1 errno 8 (Exec format error)
```

⇒ **内核交给 `report.say(why)` 的确实是 ENOEXEC(8)**,而 `translate` 把它折成 3。
反馈给的链条 8 → 3 → 5 端到端成立,两端我都独立看过。

### 1.3 三处要修正反馈的说法

**(a) 「ENOEXEC 是 `execvp` 据以回退到 `/bin/sh` 的答案」—— 对 POSIX/glibc 成立,
对本仓库里的 musl 不成立。**

`musl/src/process/execvp.c` 的 `__execvpe` 只认 `EACCES/ENOENT/ENOTDIR`,
ENOEXEC 走 `default: return -1`,**它没有 `/bin/sh` 回退**。端口自己替换的
`__posix_spawnp`(`port/src/okm_spawn.c:648`)沿用同一套规则,也没有。

⇒ 「回退不可达」在这里**不是被 EIO 破坏的,而是本来就不存在**。修好之后端口内部
没有任何控制流会变。**这一条的必要性因此不建立在「回退坏了」上,而建立在
「`kal_err_io` 是一句假话」上** —— 见 §3.2。这个区别必须写清楚,否则方案会被
写成去补一个 musl 上游有意不做的回退(§6.2)。

**(b) 「还是以 127 结束调用者」在 Linux 上确实过时,但 README 那一行不算错。**

`openkal-linux` 0.8.0(`3f16c78`「a replacement that failed is reported to the caller,
not to nobody」)就装上了报告通道。但 **openkal-macos 至今没有**
(`src/process.cpp:178` `for(;;) exit(127)`),所以那句话在 macOS 后端上今天仍然字面
成立。反馈者只测了 Linux。README 那一行的问题是**没有分后端**,不是写错。

**(c) 这不是孤例。** `translate` 的 `default → kal_err_io` 同样吞掉了 `EXDEV`、
`EMFILE/ENFILE`、`ENOTTY` 以及**全部网络 errno**。其中只有网络那一组构成实现间分歧
(§6.1),其余在三个实现之间是一致的。

---

## 2. 判据:什么算「必须」

本轮只做能指到下面三条之一的事。**指不到的一律不做,并在 §6 逐条写明为什么不做。**

| 代号 | 判据 | 出处 |
|---|---|---|
| **M1** | 某个实现今天让调用者**无声地错**(报成功却没做,或报一句关于环境的假话) | clause 3.1:*"An implementation that would leave its callers silently wrong is a simulation."* |
| **M2** | **两个实现**对同一个条件给出**不同**的 `kal_error` | clause 8:*"changing implementation is a change to one line of the manifest and to no line of the source"* |
| **M3** | 上述任一条**没有任何检查能发现它** | clause 9.1:*"Every operation the implementation declares shall behave as this specification requires."* |

M2 单独出现时**不足以决定修法**:只有在「哪个答案是对的」有明确答案时才动手。
两个都错的情况下强行统一,是把分歧换成一致的错误 —— 这正是 §6.1 不做的理由。

---

## 3. 必须做的三条

### 3.1 【M1】openkal-macos:起不来的名字返回 `kal_ok`

#### 问题的准确形状

`openkal-macos/src/process.cpp` 的 spawn:复制映像 → 在副本里 `dup2`/`fchdir`/
`setpgid` → `execve` → `for(;;) exit(127)`。父映像**没有任何通道**,直接落到:

```cpp
if (unit && join == 0) how->job->h = static_cast<kal_uintptr>(child);
*out = kal_process{ static_cast<kal_uintptr>(child) };
return kal_ok;
```

⇒ **名字不存在、是目录、没有可执行位、不是程序 —— 四种情况一律 `kal_ok` 加句柄。**

对照:

| 实现 | 失败报告 | 一个不存在的名字 |
|---|---|---|
| openkal-linux | 0.8.0 的 `exec_report` 管道 | `kal_err_not_found` |
| openkal-windows | **不需要管道** —— `CreateProcessW` 在父映像里就失败(`process.cpp:247`) | `kal_err_not_found` |
| **openkal-macos** | 没有 | **`kal_ok`** |

`openkal-macos-abi/` **不是第二个实现**:两个目录的 `mcpp.toml` 里
`name` 都是 `openkal-macos`,是同一个仓库的两个 worktree(`main` /
`abi/one-idea-one-spelling`,后者停在 openkal 0.9.0)。这条教训记在
`2026-08-30-openkal-0.10-ecosystem-plan.md` §1.1,本轮**按 `name` 去重后确认要改的
实现是一个,不是两个**。那个 worktree 本轮不动。

#### 方案:把 openkal-linux 的 `exec_report` 移过去

结构原样照搬(`openkal-linux/src/process.cpp:79-130`),只换两处这个内核不同的地方:

1. **建管道**。macOS 没有 `pipe2`,但 `src/sys.h:133` 已经有 `pipe_pair(writing)`
   —— 这个内核用两个返回寄存器交出两端。所以:

```cpp
okm_long w = -1;
const okm_long r = okm::pipe_pair(w);
if (okm::failed(r)) return false;
fd[0] = static_cast<int>(r);
fd[1] = static_cast<int>(w);
// 没有 O_CLOEXEC 可以在创建时给,所以两端各设一次。
okm::sys(okm::nr_fcntl, fd[0], okm::f_setfd, okm::fd_cloexec);
okm::sys(okm::nr_fcntl, fd[1], okm::f_setfd, okm::fd_cloexec);
```

   `f_setfd = 2, fd_cloexec = 1` 在 `src/sys.h:217` 已有,不用新增。

2. **把两端抬到 placement 之上**。Linux 用 `F_DUPFD_CLOEXEC` 找「不低于下界的最低
   空闲描述符」。macOS 的 `F_DUPFD_CLOEXEC` 是 **67**,`src/sys.h` 里还没有,需要加:

```cpp
f_dupfd_cloexec = 67,   // 这个内核的编号,和另一个内核的 1030 不同
```

   **不能用 `f_dupfd` 再补 `f_setfd` 两步代替**:两步之间另一个线程的 exec 会
   带走这个描述符,而这个管道的整个存在意义就是「被 exec 关掉才算成功」。
   Linux 侧选原子形式的理由在 `process.cpp:83-91`,这里同理。

3. 副本里 `execve` 之后 `report.say(why)`;父映像 `report.heard()`,非零则
   `reap(child)` 并 `return okm::translate(why)`。

#### 代价与边界

- 管道占两个描述符,必须抬到 `3 + grant_count` 之上,否则会被副本自己的
  placement 关掉,父侧读到 EOF 并把它当成功。Linux 侧的注释把这个坑写在
  `process.cpp:83-91`,照抄。
- 复制原语是 `okm::duplicate`(BSD 的双返回值约定),副本里 `say` 之后仍
  `exit(127)`,不变。
- **不需要新的错误值。** 补上通道之后 macOS 的映射与 Linux 今天完全一致:
  ENOENT→`not_found`、EACCES→`permission`、EISDIR→`is_directory`、
  ENOEXEC→`io`(第 2 条修完之后是新词)。

---

### 3.2 【M1】`kal_error` 加一个值

#### 为什么这是 M1 而不是「更精确一点更好」

`kal_err_io` 的定义是 *"the device or medium reported a failure"*。这条路径上
**没有任何设备报过故障** —— 文件读得好好的,内核只是不认识它的格式。所以这不是
一个粗糙的答案,是一句**关于环境的假话**,调用者据此做的任何判断都是错的。

而 SPEC 5.2 自己已经把这个论证写下来过了,一字不差地用在 `kal_err_exists` 上:

> `kal_fs_open` with `exclusive`, and `kal_fs_mkdir`, fail because the name is
> already there. It is an expected outcome, and **mapping it to `kal_err_io`
> would report a medium failure for one.**

「名字在,也允许运行,但不是这个环境能启动的形式」是同一个形状的预期结果。

#### 为什么必须是加词,而不是别的办法

- SPEC 5.2:*"Detail beyond these values is not available. A per-thread channel
  carrying the environment's own error value was **considered and rejected**"*
  ⇒ 「把原始 errno 带上来」这条路规范已经封死,不必再讨论。
- 现有十三个词里没有一个能用:`kal_err_not_supported` 会告诉调用者**别再问了**
  (这个理由 `openkal-windows/src/endpoint.h:51-54` 已经为 `WSANOTINITIALISED`
  写过);`kal_err_invalid` 说的是参数不对,而这个名字是对的;
  `kal_err_permission` 已经被 644 那个 case 占着,而两者必须分得开。

#### 命名

推荐 **`kal_err_not_program = 14`**,与 `kal_err_not_directory`(*"a directory
operation applied to a file"*)同形:一个**启动**操作被用在了不是程序的东西上。

**不用 `kal_err_not_executable`**:`executable` 在每个读者脑子里都是一个权限位,
而这个接口刚刚在 issue #28 里以「一个 FAT 卷、一个 UEFI 系统分区和一个 Windows ACL
不共享一个模型」为由拒绝了权限模型。用它会把刚推开的东西从名字上请回来。

#### 落点(五处,每处一行)

| 文件 | 改动 |
|---|---|
| `openkal/include/openkal/types.h` | `kal_err_not_program = 14, /* the name is there and may be run, but is not in a form this environment can start */` |
| `openkal/SPEC.md` §5.2 | 照 0.5 那张表的格式加一行论证(见下) |
| `openkal-linux/src/sys.h:210` | errno 表加 `e_noexec = 8`;`translate` 加 `case e_noexec: return 14;` |
| `openkal-macos/src/sys.h` | 同上(这个内核的 ENOEXEC 也是 8) |
| `openkal-windows/src/win.cpp:73` | `case ERROR_BAD_EXE_FORMAT: return kal_err_not_program;` |
| `openkal-musl/port/src/okm_fd.c:59` | `case kal_err_not_program: return ENOEXEC;` |

SPEC 5.2 那一行:

> | `kal_err_not_program` | 一个存在、可运行、而这个环境不认识其格式的名字。启动它
> 失败是一个预期结果,映射到 `kal_err_io` 会为它报一次介质故障 —— 与
> `kal_err_exists` 同一个理由。每个环境都有对应的原生条件(`ENOEXEC`、
> `ERROR_BAD_EXE_FORMAT`),所以这不是某一个内核的特产。测量来自消费者:
> openkal#28,一个 C 库因此无法把 `posix_spawn` 的失败译回 `ENOEXEC`。 |

#### 代价与边界

- **`SURFACE.txt` 不动。** 它一行一个导出名,枚举常量不在其中(已核:
  `grep kal_err_not_directory SURFACE.txt` 无命中)⇒ clause 9.2 / 9.3 的两项静态
  检查不受影响。
- **两个方向都安全。** 新端口配旧后端:后端从不产出 14,行为不变。
  旧端口配新后端:`okm_errno` 的 `default: return EIO` 接住,退回今天的行为。
- **但它触发整张图的联动**,见 §4。这是本轮唯一真正花钱的地方。

---

### 3.3 【M3】上面两条今天没有任何检查能发现

`openkal/conformance` 全部 15 个 section 加起来只有 23 处 `kal_err_*`,其中
`kal_err_io` 出现 **1 次**;`sections/process.cpp:195` 和 `:217` 检的全是「不支持的
东西有没有被拒绝」。**没有一条用例起过一个起不来的名字。**

#### (a) conformance:一条可移植的用例,直接抓 §3.1

放进 `openkal/conformance/src/sections/process.cpp`:

```cpp
// 一个起不完成的启动不得报成 kal_ok。clause 3.1:报了成功却没有程序,
// 调用者只能靠等待和读退出码来发现,而那与一个真的跑过并返回 127 的程序
// 无法区分。
{
    kal_process p{};
    const char* argv[1] = { "no-such-program-openkal-conformance" };
    const kal_uintptr lens[1] = { 35 };
    const kal_spawn how{ kal::fs::working(), kal::fs::working(), nullptr,
                         nullptr, 0, 0 };
    const int e = kal_process_spawn(&how, argv[0], 35, argv, lens, 1,
                                    nullptr, nullptr, 0, nullptr, &p);
    observe(kind::behaviour, e != kal_ok,
            "a start that did not happen is not reported as success");
    observe(kind::behaviour, e == kal_err_not_found,
            "and a name that is not there is reported as absent");
}
```

**这条用例可移植,而 ENOEXEC 那条不可移植** —— 区别值得记下来:
构造「存在但不是程序」需要在 POSIX 上设可执行位,而 `openkal.fs` **没有**这个操作
(那正是 issue #28 第 3 条的前半)。所以套件能表达的是「起不来 ⇒ 不是 `kal_ok`」,
用一个不存在的名字就够,而具体到 ENOEXEC 的那一条只能放到消费者侧(下)。

#### (b) openkal-musl probe:ENOEXEC 那一条

放进 `examples/subprocess`,CI 已经在四行上跑它
(`.github/workflows/ci.yml:629`,`tools/run-probe.sh`)。

- POSIX 行:probe 自己写一个 `#!` 都没有的一行文本文件,然后
  `posix_spawn("/bin/chmod", {"chmod","755",path})` 并等它结束 —— 端口的 `chmod`
  是 `ENOSYS`,但**起一个程序不是**,而 `--shell` 行本来就断言有 shell。
- Windows 行:没有可执行位,文件存在即可(`.txt` 后缀,避开 `startable_name` 的
  `.exe` 重试)。
- 三行断言同一件事:

```c
int e = posix_spawn(&pid, path, NULL, NULL, av, environ);
CHECK(e == ENOEXEC);   /* 不是 EIO,也不是 0 */
```

这一条**必须先在打补丁前跑一次并看它红**。`2026-08-31` 那份文档 §2 记过同样的
教训:*「能做,缺的是**能失败的测试**」*。只绿不红的用例不算数。

---

## 4. 代价:规范一动,整张图必须整体移动

依据是 `2026-08-30-openkal-0.10-ecosystem-plan.md` §0 **实测**过的规则:mcpp 的版本
要求是精确的、不向上浮动,而 `openkal` 是图里所有包的共享依赖 —— 混版得到的是
`irreconcilable versions`,不是「各用各的」。

| 包 | 现在 | 之后 | 本轮为什么动 |
|---|---|---|---|
| `openkal` | 0.12.0 | **0.13.0** | §3.2 加值 + §3.3(a) 用例 |
| `openkal-linux` | 0.12.0 | 0.13.0 | 认新值(两行) |
| `openkal-macos` | 0.9.0 | 0.10.0 | **§3.1 报告通道** + 认新值 |
| `openkal-windows` | 0.7.0 | 0.8.0 | `translate_win32` 一个 arm |
| `openkal-opensbi` | 0.6.0 | 0.7.0 | 只重钉(无 `openkal.process`) |
| `openkal-uefi` | 0.6.0 | 0.7.0 | 只重钉(连 fs 都不提供) |
| `openkal-musl` | 0.13.1 | 0.14.0 | `okm_errno` 一行 + §3.3(b) + README |
| `std-freestanding-alloc-kal` | 0.1.5 | 0.1.6 | 只重钉 |
| `openkal-llvm-runtime` | 0.9.1 | 0.9.2 | 钉 `openkal-musl = "0.13.1"`,跟着走 |
| `openkal-kit` | — | 跟 openkal 的归档 | 索引条目写明它从 openkal 的 tarball 的 `*/kit/mcpp.toml` 发布 |
| `mcpp-index` | — | 每包一条 xpm 版本 + sha256 | `pkgs/o/*.lua`、`pkgs/s/std-freestanding-alloc-kal.lua` |

清点方法按 §1.2 的教训**反向扫索引**,而不是从改动点外推:
`grep -rl openkal mcpp-index/pkgs/` 命中 17 个文件,其中 4 个 `freedesktop.*` 与
`sbase`、`std-freestanding-alloc-libc` 只是提到、不钉 `openkal`,已排除。

⇒ **十个包一起发,一个都不能落。** 这是本轮唯一的大成本,而它整个是由 §3.2 一条
带来的 —— §3.1 和 §3.3 本身不需要任何版本联动。

### 4.1 所以为什么不拆成两轮

先做 §3.1(不动规范)、后做 §3.2(动规范),听起来更稳。**但版本要求是精确的,
`openkal-macos` 单独发 0.9.1 也要求它的每一个消费者重钉一次** —— 拆成两轮等于把
上面这张表走两遍。⇒ **一轮做完,openkal 0.13.0。** 「先看红再看绿」的纪律靠本地
构建保证,不靠发布顺序。

---

## 5. 落地顺序

- [ ] **1. 先写会红的检查**(不发布)
      §3.3(a) 的 conformance 用例;本地对 **今天的** openkal-macos 跑,
      必须报「a start that did not happen is not reported as success」失败。
      ⇒ 这一步同时**证实** §3.1 的读数,把它从「读源码得出」变成「测到」。
- [ ] **2. openkal-macos 报告通道**(§3.1)。用例转绿。此时还没有新错误值,
      ENOEXEC 在 macOS 上和 Linux 一样折成 `kal_err_io` —— 这是预期的中间态。
- [ ] **3. openkal 0.13.0**:`types.h` 加值 + `SPEC.md` §5.2 加论证行 + 用例入库。
      核对 `SURFACE.txt` 无需改动。
- [ ] **4. 三个实现认新值**:linux / macos 各两行,windows 一行。
- [ ] **5. openkal-musl**:`okm_errno` 一行 + §3.3(b) 的 probe 用例
      + README 限制表那一行改写(见 §5.1)。
- [ ] **6. 重钉与发布**:按 §4 的表,十个包 + 索引。
- [ ] **7. 闭环**:干净沙箱 + CN 镜像,只写版本号解析一遍(0.10 那轮的 §5.1 流程)。

### 5.1 README 限制表那一行怎么改

现在这一行(`README.md:163`)对 Linux 已经是假的,对 macOS 曾经是真的:

> | whether a file may be executed | `access(path, X_OK)` answers **yes for anything
> that exists**, and starting a name that exists and cannot be run still ends the
> caller with 127 | … |

改成分成两半、并标出各自的归属:

> | whether a file may be executed | `access(path, X_OK)` 仍然对任何存在的名字答
> **yes** —— 这一半没有答案,`kal_node_info` 只带 `writable`。**另一半自 0.14.0
> 起答了**:起一个存在而起不来的名字**报错并且不返回 pid**,理由完整传上来
> ——`ENOENT`、`EACCES`、`EISDIR`、以及 `ENOEXEC`(openkal 0.13 的
> `kal_err_not_program`)。0.13.1 及以前把最后一种报成 `EIO`;而在
> openkal-macos 0.10.0 以前,**四种一律报成功**,调用者只能等到 127 |

---

## 6. 明确不做,以及为什么

### 6.1 网络错误值:三个实现今天不一致,而本轮仍然不动

**这是核实过程中挖出来的、比 ENOEXEC 更普遍的一条**,也是本轮最难的一个判断。

`openkal-windows/src/endpoint.h:35` 有一整张 `translate_wsa`,注释自陈其存在理由:
*"Passing one of them to `translate_win32` produces `kal_err_io` for all of them,
which is a mapping that compiles, runs, and tells a caller nothing."*
而 `openkal-linux/src/sys.h` 与 `openkal-macos/src/sys.h` **一个网络值都没有**
—— 连常量都没定义,全部走 `default → kal_err_io`,`net.cpp` 用的正是这个 `translate`。

| 条件 | Windows | Linux / macOS |
|---|---|---|
| 连接被拒 | `not_found` | `io` |
| 主机 / 网络不可达 | `not_found` | `io` |
| 地址已占用 | `exists` | `io` |
| 连接进行中 | `again` | `io` |
| 不是套接字 / 地址族不支持 | `invalid` | `io` |
| 描述符耗尽(套接字) | `no_space` | `io` |
| 连接被重置 | `closed` | `closed` |

这满足 **M2**。但 §2 写明了 M2 单独出现不足以决定修法,而这里恰好就是那种情况:

**把 Linux 改成跟 Windows 一致,是把一个错答案换成另一个错答案。**
`kal_err_not_found` 经 `okm_errno` 是 **`ENOENT`**。一个 C 库程序连一个没人监听的
端口,今天拿到 `EIO`,改完拿到 `ENOENT` —— 而它要的是 `ECONNREFUSED`。
所有检查 `errno == ECONNREFUSED` 的重试循环,改前改后一样不触发;而
`ENOENT` 对一个套接字来说比 `EIO` 更容易把人引到错误的方向(去找一个不存在的路径)。

⇒ 这一组的**正确**修法是给 `kal_error` 加网络词(refused / unreachable / in-use),
那是一次远大于本轮的接口变动,而**今天没有任何消费者测量支撑它**。

**本轮的处理:记录,不动手。**
- 在 openkal-musl `README.md` 限制表加一行,写明「网络失败的原因在这个接口里没有
  词,三个实现的答案还不一致」,并把两张表并列指出来;
- 在 issue #28 的回复里把它作为「同一形状的第四条」提出,等一次真实测量。

这和 §3.2 的区别正是本轮的分界线:`ENOEXEC → kal_err_not_program → ENOEXEC`
是**一一对应、无判断余地**的;网络那一组的每一个候选词都要做判断,而判断需要
测量。**能一一对应的加词,需要判断的等测量。**

### 6.2 musl 的 `execvp` 不补 `/bin/sh` 回退

POSIX 要求 `execvp` 在 ENOEXEC 时改用 shell 运行;musl 上游有意不做
(`musl/src/process/execvp.c` 的 `default: return -1`)。补它会让这个端口和它所移植
的 C 库在同一个函数上分家。`musl/PATCHES.md:93` 已经为**同一类问题**做过一次决定
(PATH 分隔符即使在一个环境上是错的也保持冒号,理由是「两种搜法互相矛盾比
一起错更糟」)。⇒ 同一条理由,不补。

### 6.3 `access(X_OK)` 的询问半边

issue #28 第 3 条的前半,需要接口获得一个权限词,而那在 issue 里已经被否决且理由
成立。反馈者自己也明确说「这一半我没有测量可提供」。⇒ 不做,README 那一行继续
如实记着它没有答案(§5.1)。

### 6.4 `EXDEV` / `ENOTTY` / `ERANGE` 加词

`EXDEV` 在 Windows 上是 `ERROR_NOT_SAME_DEVICE`,`translate_win32` 也没有 arm
—— **三个实现在 `kal_err_io` 上是一致的**,不构成 M2;也没有任何测量。
⇒ 不满足任何判据,不做。

### 6.5 `WSAEINTR → kal_err_again` 与 clause 7.5 的张力(只记录)

clause 7.5 要求被中断的调用**重试而不是上报**;`endpoint.h:42` 把 `WSAEINTR`
报成 `kal_err_again`。现代 Winsock 不再产生它(`WSACancelBlockingCall` 在
Winsock 2 已移除),所以既无测量也无实际后果。⇒ 记在这里,本轮不动。

### 6.6 `openkal-macos-abi` worktree

同一个包的另一个分支,停在 openkal 0.9.0。本轮不动;它跟上来的时候会一起带上
§3.1 的改动,因为那是同一个仓库的 `main`。

---

## 7. 需要 review 决定的三点

1. **新值叫什么。** 我推荐 `kal_err_not_program`(与 `kal_err_not_directory` 同形)。
   反对 `kal_err_not_executable` 的理由见 §3.2,但如果你认为「executable」在这里
   足够清楚,这是一个可以推翻我的地方。

2. **§6.1 记录而不动,是否接受。** 三个实现今天在网络错误上不一致是**事实**,
   我判断动它会让消费者从一个错答案换到另一个错答案。如果你认为「一致」本身
   就值得先拿到,那 §6.1 要从「不做」移到「做」,`openkal-linux/src/sys.h` 与
   `openkal-macos/src/sys.h` 各加约 12 个 case。

3. **是否一轮做完。** §4.1 论证了拆两轮反而更贵(精确版本要求 ⇒ 走两遍全图)。
   如果你希望 §3.1 先落地救 macOS,那就是接受走两遍。

---

## 8. 这份方案自己的边界

- §3.1 的结论**目前是读源码得出的,还没跑过**。落地顺序第 1 步就是把它变成一次
  真实的失败读数;如果那一步没红,§3.1 整条要撤回重写。
- §1.2 的控制读数是在 **host glibc 2.44 / x86_64** 上取的,不是在这个端口里取的。
  端口侧的 `EIO` 是反馈者测的,我只核了源码链条。§3.3(b) 的 probe 用例会把这一半
  也变成本仓库自己的读数。
- 反馈里「一个 755、非 ELF、无 `#!` 的文件」在 **aarch64 + binfmt_misc** 上可能匹配
  到别的处理器而不是 ENOEXEC。probe 用例要用一行普通文本(不以 `#!` 开头、不是
  任何已注册格式),并且**只在 CI 的四行上断言**,不对其他环境作承诺。
