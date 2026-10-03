# Stellaris 4.5.x 大页（2 MB Large Pages）补丁 —— 重写版 v2（已验证：零崩溃）

> 目标：让群星 4.5.1 真的把内存分配在 2 MB 大页上。
> 做法：**放弃 IDA 注入器路线**，改为给 `stellaris.exe` 静态打一个最小补丁，让游戏自己加载我们的分配器 DLL。

本目录所有内容都在你这台机器上实测跑通。**v2 已经消灭了上一版的 2 条 WER 崩溃事件。**

---

## 1. 快速使用

```powershell
python .\patch_stellaris.py status     # 看状态
python .\patch_stellaris.py install    # 打补丁（用同目录 lpshim.dll）
# 正常启动游戏（Steam / 双击 / 启动器都行），玩一会儿
notepad .\lpshim-log.txt               # 看日志
python .\patch_stellaris.py restore    # 一键逐字节还原
```

Steam「验证游戏文件完整性」也会自动抹掉补丁，等于自带保险。

### 配置 `lpshim.cfg`（和 DLL 同目录，当前已是推荐值）

| 键 | 当前值 | 说明 |
|---|---|---|
| `ArenaGB` | 2 | 大页竞技场大小（GiB）。大页**不可换页**，会占住物理内存；无模组 2 GB 足够（实测峰值用量 ~1 GB） |
| `ThresholdMB` | 1 | 多大的分配才走大页。**别低于 1**（0.25 会失控） |
| `HookVA` | **0** | 关掉 VirtualAlloc 挂钩。实测群星几乎不用 VirtualAlloc（总 323 次/0.3 MB），但挂上它会产生每秒百万级的分配抖动，故默认关闭 |
| `HookHeap` | 1 | 挂钩 `HeapAlloc/HeapFree/HeapReAlloc/HeapSize` + ntdll 的 `Rtl*Heap`（所有模块，含后加载的模块，每 3 秒重扫） |
| `VerboseAlloc` | 0 | 1 = 每次大页分配都写日志（排查用） |
| `Log` | 本目录 `lpshim-log.txt` | 日志路径 |

---

## 2. 实测证据（无模组、ArenaGB=2、HookHeap=1、90 秒）

```
arena: RESERVED 2048 MB with MEM_LARGE_PAGES at 0000027200000000 (2MB-aligned=1)
verify: arena data at ...10 -> wss 0x0000000000C00041 (Valid=1 LargePage=1)
stats: HeapAlloc=22.6M(12560MB) big=877(4626MB) | arenaServed=877(4626MB) fail=0 LIVE=973.1MB peak=979.4MB
```

| 指标 | 结果 |
|---|---|
| 游戏进程 | **1 个**（不再出现会崩的辅助子进程） |
| WER 崩溃事件 | **0 条** ✅ |
| 大页验证 | `LargePage=1`（由 `K32QueryWorkingSetEx` 由操作系统确认） |
| 走大页的分配 | 877 次，累计 4626 MB，失败 0 次 |
| **当前常驻大页内存** | **973 MB（峰值 979 MB）** |
| 游戏内存 / 运行 | 9.08 GB / 90 秒无异常 |

---

## 3. 两个"坑"是怎么被定位并修掉的（v2 的核心工作）

**症状**：v1 每次运行都会多出一个 `stellaris.exe` 辅助子进程，1 秒后崩溃，产生 2 条 `ntdll c0000005 @1a9dd` 事件。主进程始终正常。

逐步二分（每次都做对照，全部无模组）：

| # | 实验 | 子进程 | 崩溃事件 |
|---|---|---|---|
| 1 | 空 DLL（什么都不做） | 无 | 0 |
| 2 | 完整 shim（挂钩全部 IAT） | 有 | 2 |
| 3 | **一行 IAT 都不改**（HookVA=0/HookHeap=0，只加载+预留大页） | 有 | 2 |
| 4 | `Minimal=1`（DLL 只加载+写一行日志） | 有 | 2 |
| 5 | 延迟 2.5 秒再初始化 | 有 | 2 |
| 6 | 微探针：空 DLL **+ `CreateThread`** | **有** | **2** |
| 7 | 微探针：空 DLL **+ `CreateMutexA`** | 无 | 0 |

→ **根因：在"入口点"这么早的时机创建线程**，会让群星在 T+0.2 秒启动一个自己的辅助副本进程，该副本随后崩溃。（那个副本并不加载我们的 DLL —— `attach-log` 只有主进程，所以问题不在我们的代码里，而在我们扰动了它的启动时序。）

**修复**：v2 的 shim **完全不创建线程**——所有工作（开权限、预留大页、挂钩 IAT）都在主线程同步完成；周期性的模块重扫和统计输出改为在挂钩函数内部"惰性"触发（每 3 秒重扫一次新加载的模块、每 15 秒打一行统计）。

上表第 6/7 行就是修复的验证：只要不建线程，辅助子进程根本不出现，崩溃事件为 0。

---

## 4. 为什么旧方案（LargePageInjectorMods）在 4.5.1 上必挂

逐条都有对照实验（这些是带 80 个模组时做的）：

| 实验 | 内容 | 结果 |
|---|---|---|
| A | 正常启动，不注入 | ✅ 存活 90 s / 5.1 GB |
| B | 注入一个**只写一行日志的空 DLL** | ❌ T+2.2 s 崩溃 |
| C | 原版 mimalloc，hook 全关、大页 0 | ❌ T+1.4 s 崩溃 |
| D | 你的原配置（hook + 12 GB 大页） | ❌ T+12.3 s 崩溃 |
| E | **把 DLL 换成根本不是 PE 的垃圾文本文件**（`LoadLibraryW` 必然失败，进程里从未出现任何模块） | ❌ **T+1.5 s 崩溃** |
| F | DLL 把自己从 PEB 模块链表摘掉 | ❌ 仍崩 |
| G | DLL `return FALSE` 让加载被完全回滚 | ❌ 仍崩 |

**E 是决定性的**：连一个外来模块都没出现，游戏照样 1.5 秒死。所以崩溃与 mimalloc、那 12 个签名、大页配置**毫无关系**——是 `Injector.exe` 那套"挂起创建 + 远程线程 LoadLibrary"的启动/注入流程本身在 4.5.1 上就把游戏搞死了。修 hook、换签名、调大页参数都不可能救活它。

（附带纠正一个我中途的错误结论：`stellaris.exe` 开了 **CFG**，我早期用 `jmp rax` 间接跳转触发了 CFG 违规，一度误判成"改 exe 会被防篡改杀掉"；换成直接相对跳转 `E9` 后，打补丁的程序行为与原版完全一致。）

---

## 5. 已知局限

1. **只支持 4.5.1 (358e)**：补丁依赖入口第一条指令是 `call rel32`、`LoadLibraryA` 的 IAT RVA = `0x2216390`。版本不符时 `install` 会拒绝执行（宁可不动也不写坏文件）。
2. 改了 exe 之后**联机校验值会变**，多人游戏大概率不可用。
3. 游戏更新会覆盖补丁，需重新 `install`。
4. 只改 `stellaris.exe` 一个文件；不碰存档、mod、Steam 文件、注册表、启动器配置。
5. 覆盖率：只接管 ≥1 MB 的分配（约 1 GB 常驻）。想覆盖小分配就得换真正的通用分配器（mimalloc 等），但那会引入"释放路径不匹配"的风险——这就是 v1 时代那些诡异现象的来源，所以本版选择"少而稳"。
6. 这次你关掉了 80 个模组：`ArenaGB=2` 是为此调的。如果以后又把模组全开，建议改成 `ArenaGB=4`（模组配置下实测常驻大页可达 1.6–1.8 GB）。

## 6. 目录内容

| 文件 | 说明 |
|---|---|
| `patch_stellaris.py` | 自包含安装/卸载/状态工具（含版本校验） |
| `lpshim.dll` | 载荷（v2，无线程版，已验证零崩溃） |
| `lpshim.c` | 源码（tcc 编译：`tcc -shared -o lpshim.dll lpshim.c -ladvapi32`） |
| `lpshim.cfg` | 配置（推荐值已写入） |
| `lpshim-log.txt` | 上次运行的日志（含 `LargePage=1` 与大页用量） |
| `backup/stellaris.exe.orig` | 原始 exe 备份（sha256 `6fe06709f265e726`） |

## 7. 卸载

```powershell
python .\patch_stellaris.py restore
```
或 Steam「验证游戏文件完整性」。
