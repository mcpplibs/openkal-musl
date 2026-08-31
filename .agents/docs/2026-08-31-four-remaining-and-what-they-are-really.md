# 0.11 之后剩下的四条:方案与设计

> 状态:**待 review,一行代码都还没写。**
>
> ⚠️⚠️ 这份文档的第一个结论是:**四条里只有一条是「待做的工作」,另外三条分别是
> 「已经能做但没做」「已经做对但没被指出来」「已经处理正确只差一句话」。**
> 而我在写它的过程中提过一个错的方案,那个错法本身比方案更值得记——见 §3.1。

---

## 0. 一句话总览

| # | 事项 | 真实性质 | 动规范吗 | 落点 |
|---|---|---|---|---|
| 1 | `fork(); setpgid(0,0); exec` 够不着 | ⭐ **现有操作就能做,只是没接上** | ❌ | openkal-musl |
| 2 | macOS 不 claim `stop_requested` | 能做,缺的是**能失败的测试** | ❌ | openkal-macos |
| 3 | 权限模型 | ⭐⭐ **规范已经答对了**,缺的是指路 | ❌ | fs.h 注释 + musl README |
| 4 | `openkal-libc` 双条目 | 索引自洽,只有一个松头 | ❌ | 一句注释 |

⇒ **零 ABI 变动。** 这不是巧合——三条的「缺口感」都来自我从**需求**倒推,而不是从
**接口已有什么**出发。

---

## 1. `fork(); setpgid(0,0); exec`

### 1.1 问题的准确形状

副本 `setpgid(0,0)` 形成的单位,身份是**副本的真实标识**;父进程手里是这个 C 库
分配的号。两者不是同一个东西,所以父侧 `kill(-n)` 指向一个不存在的组。

⚠️ 我之前把它记成「够不着,需要新声明」。**那是错的**:缺的不是「命名单位的能力」,
是**副本把身份告诉父进程的通道**。

### 1.2 ⭐ 而那条通道 openkal 已经有,并且正是为这件事存在的

`kal_process_channel` 的声明处写着:

> the mechanism a parent uses to speak to a child it started --- which openkal
> otherwise has no way to express

### 1.3 方案

```
fork      父 kal_process_channel(&mine, &theirs);mine 记进 g_child[slot]
          副本持有 theirs
setpgid   副本 job_enter 之后,把 unit.h 写进 theirs
kill(-n)  父按 n 找到 slot,对 mine 做一次有界读
            读到值  → job_terminate(那个单位)
            读到 EOF → 副本没形成单位
```

⭐ **EOF 天然表示「没有单位」**,因为本轮刚加的 `__okm_close_all_for_exec` 会在
exec 前关掉副本持有的一切。两个改动正好咬合——这不是设计出来的,是发现的。

### 1.4 代价与边界

- 每次 `fork` 多两个流句柄。⚠️ openkal-windows 本就不提供 `openkal.space`,不受影响。
- 读**必须有界**(`openkal.timeout`),否则副本还没 `setpgid` 时父进程会阻塞。
- ⚠️ 副本形成单位与父进程询问之间有窗口。**这个窗口在真 Unix 上也存在**——消费者
  代码里那句「父侧同步 setpgid 关竞态窗」就是在处理它。不是新问题,而且这个方案
  下父进程可以**重试读**,比 Unix 那边的重发 `setpgid` 更直接。

### 1.5 怎么验证

现成的:`/tmp/jobp` 那个探针改回 `fork` 形态。⚠️ **必须双向**——摘掉这个改动之后
`kill(-n)` 回到 ESRCH、后台的 `sleep` 存活。

---

## 2. macOS 的 `stop_requested`

### 2.1 为什么现在不 claim

这个内核的 `sigaction` 收的 `struct __sigaction` 带 `sa_tramp`,内核用它从 handler
返回;C 库平时提供 `_sigtramp`,而 openkal-macos 底下**没有 C 库**。

⚠️ 装错的表现是「程序以没人能追查的方式死掉」——这正是本轮 SIGPIPE 那条缺陷的
形态,从另一侧遇到。所以「没测过就不 claim」不是保守,是这个仓库的既有标准。

### 2.2 方案是两半,**顺序不能反**

**(b) 先写一个会失败的测试。** 不是「能编过」,是**真的发一次信号**:

```
安装 → 对自己发 SIGTERM → 断言 ①字被置位 ②程序还活着 ③跑到结尾
```

⭐ 就是 Linux 上那个探针(`ok: told, and still running`)。openkal-macos 的 CI 跑
`macos-14`,所以**它能被测**,只是我这台机器不能。

**(a) 再写 trampoline。** 内核以 `(handler, infostyle, sig, siginfo, ucontext)`
调用它;它调 `handler(sig, siginfo, ucontext)`,然后 `sigreturn(ucontext,
infostyle)`(syscall 184)。arm64 上参数在 x0–x4:把 x2/x3/x4 挪到 x0/x1/x2 调
handler,用保存下来的 x4/x1 调 sigreturn。约十条指令,和 openkal-linux 上那三条
x86_64 指令是同一类工作。

⭐ 而且**只需要 arm64 一份**:那个矩阵只有 `macos-14`,注释写明了原因——构建工具
在这个系统上没有 x86_64 发布。所以这不是「先做一半」,是全部。

⚠️ **先 (b) 后 (a)**,否则又是一次「编过了所以以为对了」。

---

## 3. 权限模型 ——⭐⭐ 规范已经答对了

### 3.1 ⚠️⚠️ 我先提了一个错的方案,记在这里

我提过 `KAL_OPEN_PRIVATE` + `kal_fs_mkdir_private`,论据是「『只有我能读』两边都能,
是真正通用的原子能力」。

**它是错的,而且错在 ABI 层面:「private」预设了一个「me」,而 openkal 没有「me」。**

`kal_fs_open(dir, name)` 能到达什么,完全由持有哪个 `kal_dir` 决定;不存在一个
环境级主体供内核比对。那个旗标只有把宿主的模型泄进来之后才有意义——**正是
clause 7.1 判定的借形状**。

⇒ 我是从**需求**倒推的(消费者要 0600),不是从**接口建模了什么**出发。这条记在
这里,因为它是本轮唯一一次我自己走进 clause 7.1。

### 3.2 各 ABI 实际建模的是什么

| ABI | 建模对象 | 检查发生在 |
|---|---|---|
| POSIX | 文件带 owner/group + 9 位;进程带 **ambient uid/gid** | 拿环境级身份比对 |
| Windows NT | 对象带 security descriptor;线程带 **access token** | 拿令牌比对 ACL |
| **WASI** | ⭐ **没有权限模型**:`filestat` 无 mode/uid/gid,`path_open` 不收 mode | 不比对——只能到达被授予的 |
| Zircon / seL4 / Capsicum | handle + rights | 同上 |

⭐⭐ **关键不是「哪些位」,是有没有 subject。** openkal 和 WASI 站在同一个立场上。

### 3.3 ⭐ 而 WASI 恰好证明这条线画在哪

WASI **有** `fs_rights_base` / `fs_rights_inheriting`——但那是**挂在句柄上的能力**
(这个句柄允许做什么),不是**挂在文件上的权限**(别人允许做什么)。

- 句柄权利:随**授予**流动,无需身份 ⇒ **openkal 已有**(`KAL_OPEN_READ`/`WRITE`)
- 文件权限:随**身份**判定,必须有主体 ⇒ **openkal 正确地没有**

### 3.4 规范已经写了,而且写得比我好

clause 11 第 6 条:

> **Permission and ownership of files.** Not defined, and not a deferral. A
> permission presupposes an identity, and the environments this specification
> targets do not agree that one exists.

并且按**威胁来源**给了三条替代路径:同程序的另一部分靠能力本身;机器上的另一个
用户靠**启动方给的 preopen**;不信任的位置靠加密。

⭐ 第二条是要点:在能力模型里,「让这个文件私有」**不是对文件的操作**,是**你被授予
的那个目录的属性**。授予者知道机器上有谁,程序不知道。

### 3.5 那么真正要做的两件小事

⚠️ 核实过:`fs.h` 里**确实有**指向 clause 11 item 6 的交叉引用——在 `KAL_INFO_IDENTITY`
(102 行)和 `kal_fs_lock`(328 行)处。**但不在 `kal_fs_open` 和 `kal_fs_mkdir` 处**,
而那两个才是「我要建一个私有文件」的人会看的地方。

**① fs.h:在两个 CREATE 操作处加交叉引用。** 一句话,指向 clause 11 item 6 的三条
替代路径。⚠️ 我是读了消费者代码才发现这个缺口,不是读了 fs.h——那就是证据。

**② openkal-musl README:限制表里 `fchmodat`/`access(X_OK)` 那两行补上替代路径。**
⚠️ 消费者会读 README,不会读 SPEC。现在那两行只说了「不能」。

### 3.6 可执行位:同理,而且更彻底

Unix 的 `+x` 是拿 ambient 身份判的权限位;Windows 根本不用位判(按扩展名);
WASI 连 exec 都没有。⇒ **没有可映射的原子能力**,openkal 正确地不建模。程序要
**运行**某个东西,靠的是被授予了含它的目录。照 §3.5 ② 一并记进限制表。

---

## 4. `openkal-libc` 双条目 ——索引是对的

### 4.1 核实结果

| | 版本范围 |
|---|---|
| `openkal-libc.lua` | 到 **0.2.0** 为止 |
| `openkal-musl.lua` | 从 **0.3.0** 起 |

连续、不重叠。索引注释写明了理由:

> a package that restarted at 0.1.0 would give one tag two meanings … The
> earlier name keeps its own descriptor so that a project pinned to it continues
> to resolve, and nothing is added to it.

⇒ **这是有意的重命名,处理得正确。** 我先前说它是「同一个包的两个条目」,说重了。

### 4.2 唯一的松头

`mcpplibs/openkal-libc` **仓库**仍在收到发布(0.12.0,与 openkal-musl 同一分钟,
说明有自动镜像)。索引不引用它,所以**无害**;但与注释那句「nothing is added to
it」不一致。

### 4.3 方案,按代价从小到大

- **(a) 加一句注释**,说明仓库仍被镜像而索引有意停在 0.2.0。把「不一致」变成
  「已知且有理由」。
- **(b) 停掉那条镜像**——先要找出是什么在推。
- **(c) 归档仓库**,README 指向 openkal-musl。最干净,但会断掉直接克隆它的人。

⇒ **建议 (a),并顺手查 (b) 的来源。** 这条不值得冒险,而「不一致但有记录」远好过
「不一致且无人知道」。

---

## 5. 落地顺序

1. **§1**(openkal-musl):收益最大、零规范风险、可双向验证
2. **§3.5**(fs.h + README):两处注释,零 ABI 变动
3. **§4.3 (a)**(mcpp-index):一句注释
4. **§2**(openkal-macos):先测试后实现,由 CI 的 macos runner 判定

⚠️ 1–3 可以并行且互不依赖;4 单独走,因为它是这台机器**验证不了**的唯一一条。
