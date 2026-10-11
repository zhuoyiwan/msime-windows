# 问题分级与二改路线（v2 · 目标对齐版）

- 目标：① 先修 bug ② Android 版 ③ 多设备剪贴板共享
- 依据：`docs/issues-triage.md`（2026-10-11 上游 issue 快照）
- 本版变化：按三目标重排；与目标无关的 enhancement 一律降级
- 说明：F 值为基于当前代码结构的**估算**，未实测；代码事实均已核对仓库实际文件

## 0. 分级方法

维度沿用 v1（S 严重度 / A 影响面 / F 成本），新增 **G 目标对齐度**：

| G | 含义 |
|---|---|
| G1 | 直接服务目标②或③ |
| G2 | 间接支撑（Android 或同步也要用到的能力） |
| G3 | 与三目标无关 |

排序规则：**bug 一律优先（目标①）**；enhancement 按 G1 > G2 > G3，G3 降为「接受外部 PR，不主动投入」。

## 1. ⚠️ 先决判断：目标② 可能不该在本仓做

已核实的事实：

- 组织内存在 `metasequoiaime/msime`，官方定位是「**下一代跨平台水杉输入法（MSIME）：面向 Android、iOS、macOS、Linux、Windows 与 HarmonyOS 的多平台中文输入法，使用共享输入引擎与各平台原生宿主**」。
- 该仓技术栈是 **Rust + TypeScript**（有 `Cargo.toml`、`pnpm-workspace.yaml`），顶层为 `crates/`、`apps/`、`platforms/`、`packages/`、`shared/`。
- **`platforms/android` 已经存在**（main / develop / master 三个分支都有），同级还有 `ios`、`harmony`、`macos`、`linux`、`windows`、`common`。
- 原 `msime-linux` 仓已归档，注明「已迁移到 msime 主仓」。
- `msime-cloud` 是 Go 共通后端（云候选、AI 联想、翻译、语音识别、Swagger 文档）。

本仓 `msime-windows` 是 **C++ / Win32 / WebView2** 技术栈，与 `msime` 的重写栈不同。因此目标②有两条互斥路线：

| 路线 | 做法 | 优点 | 代价 |
|---|---|---|---|
| **A（推荐）** | 在 `msime` 仓或其 fork 上推进 Android 宿主 | 复用上游已有的 `platforms/android`、Rust 引擎与跨平台契约，不重复造轮子 | 需先熟悉 Rust/TS 栈；与修 bug 分属两个仓库 |
| B | 在本仓把 C++ 引擎用 NDK 编到 Android，自建宿主 | 与现有 C++ 引擎、词库产物同源 | 与上游重写方向分叉；Android 宿主/JNI/UI 全部自建；后续难与上游同步 |

**结论**：已选定 **路线 A**（在 `msime` 仓推进 Android 宿主）。因此本仓的 E1–E6 **不再需要**，本仓路线收敛为「修 bug（目标①）+ 剪贴板同步客户端（目标③）」。

### 路线 B 的可行性审计（已核实）

- `engine/docs/runtime-architecture.md` 明确：Engine 只负责可移植输入行为，平台宿主负责原生按键 / UI / 权限 / 网络 / 插入文本；新平台入口是 `<metasequoia/session.h>` 并链接 `MetasequoiaIme::Engine`（`engine/CMakeLists.txt:129`）。
- engine 一方源文件 **266 个**（已排除第三方），Windows 专属引用仅 **25 处**：
  - `engine/voice/platforms/windows/*` 8 处 —— 已按平台隔离，Android 需新增 `platforms/android/`
  - `engine/voice/third_party/miniaudio` 8 处、kenlm / googlepinyinime 若干 —— 属第三方
  - **核心阻塞只有 2 个文件**：`engine/core/data_path.h`、`engine/core/key_event.h`，二者均 `#include <Windows.h>`，且被约 24 处 include 链波及
- 已有可照抄的平台分支范式：`engine/japanese/japanese_sentence_decoder.cpp` 使用 `#ifdef _WIN32`。
- 资源与用户数据：`prepare_runtime_paths(resource, user, cache, digest)` 要求三棵目录树互不包含、路径必须绝对 —— Android 可直接映射到 app 私有目录。
- 候选窗 UI 可复用 `ui-html/webview2/candwnd` 与 `shared/`（由 `engine/contracts/webview/` 生成），Android 用 WebView 承载。

## 2. 目标①：修 bug（沿用 v1 分级，收敛为批次）

### P0 阻断（6 项，先修）

| # | 问题 | 主要落点 | F |
|---|---|---|---|
| 708 | csgo2 游戏内打字→切屏卡死闪退 | `server/src/window/window_hook.cpp` | F2 |
| 797 | 文件移动/扫描时输入法无法输入 | `server/src/ipc`、`server/src/session` | F1 |
| 701 | 设置窗口一直转圈无法设置 | `server/src/settings/settings_app.cpp` | F1 |
| 68 | 安装向导无路径；WebView2 缺失时候选窗 cloak | `installer/`、`server/src/webview2/` | F1 |
| 296 | Win11 无法打出中文、无候选框 | `windows/src/Register`、`windows/src/IPC` | F2 |
| 421 | 装完后系统自带日语输入法候选词消失 | `windows/src/Compartment`、`windows/src/Tf` | F1 |

### P1 核心输入路径（18 项）

**2.1 候选窗显示族（12 项，同源，建议一个专项）**
93、618、788、698、387、63、733、781、783、722、680、772
→ 共用 `server/src/window/candidate_presenter.cpp` + `ime_windows_candidate_show.cpp` 生命周期；未合并 PR **#834** 就在这条线上。

**2.2 引擎切分 / 模糊音 / 辅助码族（6 项）**
718、803、707、710、671、509
→ `engine/core/input_session_composition.cpp`、`engine/quanpin/`、`engine/direct_helpcode/`

### P2 场景 / 性能 / 宿主兼容（13 项）

- 日语族：662、682、786、741 → `engine/japanese/romaji_converter.cpp`
- 快捷键拦截：762、188 → `windows/src/Key`
- 宿主兼容：688、689、60、818、773
- 渲染细节：505、58、438

### P5 清理（不投入开发）

`needs-info` 42/68/296；`can't reproduce` 58/296/387；`wontfix` 未关 644；无标签 831/748/665。

## 3. 目标②：Android

### 相关 issue 重排

| issue | 原级 | 新级 | 理由 |
|---|---|---|---|
| 761 注音输入模式 | P4 | **G1** | 台湾用户明确刚需，移动端权重高 |
| 759 / 33 符号输入混输 | P4 | **G1** | 移动端无实体键盘，更依赖符号面板 |
| 734 / 21 网格、多列候选 | P4 | **G1** | 触屏更适合网格布局 |
| 805 竖屏优化 | P4 | **G1** | 手机以竖屏为主 |
| 615 Rime 支持 | P4 | G2 | 跨平台输入方案 |
| 744 / 663 自定义方案 | P4 | G2 | 移动端换方案需求 |
| 754 / 634 / 660 / 664 五笔 | P4 | G2 | 同上 |
| 235 纯本地模式 | P4 | G2 | 移动端流量与隐私 |
| 730 插件系统 | P4 | G3 | 架构重，暂缓 |

### 使能工作（因选定路线 A，以下 E1–E6 在本仓**不再需要**，移至 `msime` 仓推进）

| # | 任务 | 落点 |
|---|---|---|
| E1 | 解除 engine 核心 Windows 依赖 | `engine/core/data_path.h`、`engine/core/key_event.h` |
| E2 | 新增 Android 语音平台实现 | `engine/voice/platforms/android/` |
| E3 | Android 宿主（IME Service + JNI） | 路线 A：`msime/platforms/android`；路线 B：新仓 |
| E4 | Android 构建（NDK + CMake、SQLite 与资源包 ABI） | 新增构建脚本 |
| E5 | 候选窗 WebView 承载 + 契约同步 | `ui-html/webview2/candwnd`、`shared/` |
| E6 | 词库与资源包适配 Android | `product-lock.json` 的 `dict-*` release |

### 风险

- 词库 / 资源包目前是 Windows 产物，Android 需重新打包或复用同一份数据。
- 语音模块的 Windows 实现不可用，Android 必须新写平台实现。
- 路线 B 会与上游 `msime` 重写分叉，长期维护成本高。

## 4. 目标③：多设备剪贴板共享

### 现状（已核实）

- `server/src/clipboard/clipboard_history.{h,cpp}`：**仅本地历史**，文件存储，上限 50 条 / 4000 字符，接口用 `std::wstring`；同目录只有 `ClipboardMonitor::{Start,Stop,Sync}`，**没有任何传输层**。
- `server/src/cloud/`：现有云端客户端（云候选、翻译），是接同步协议的现成位置。
- `msime-cloud`：Go 共通后端（云候选、AI、翻译、语音识别、Swagger）→ **同步服务的天然落点**。

### 相关 issue 重排

| issue | 原级 | 新级 | 理由 |
|---|---|---|---|
| 678 同步功能（WebDAV / Rime 式） | P4 | **G1** | 直接服务目标③，且明确要求含配置 + 词库 |
| 712 手机电脑同步复制 | P4 | **G1** | 目标③的字面需求 |
| 811 剪切面板独立到工具导航条 | P3 | G2 | 剪贴板 UI 前置 |
| 826 快捷短语支持多行 | P4 | G2 | 剪贴板文本多为多行 |
| 646 / 645 短语与词库的删除 | P4 | G2 | 与 678 的「词库同步」同域 |
| 798 统计同步 | P4 | G3 | 走同一通道但非剪贴板 |

### 使能工作（需新建）

| # | 任务 | 落点 |
|---|---|---|
| S1 | 数据模型改造：条目加 id / 时间戳 / 来源设备 / 哈希 | `server/src/clipboard/clipboard_history.*` |
| S2 | 局域网同步协议与端点 | `engine/contracts/` 定义契约；Windows 实现在 `server/src/sync/` |
| S3 | Windows 端局域网同步器 | 新增 `server/src/sync/` |
| S4 | 安全：设备配对 + 端到端加密 + 按设备开关 | 新增 |
| S5 | UI：设备列表与同步开关 | `ui-html/webview2/settings/` |
| S6 | Android 端接入 | `msime` 仓（路线 A） |

### 待定决策

1. **传输（已定）**：v1 仅局域网直连。
2. **范围（本轮）**：剪贴板记录模型；配置、词库与短语同步另排。
3. **安全**：剪贴板内容高度敏感，必须端到端加密 + 设备配对，不能明文上云。
4. **冲突策略**：多设备并发写入的合并规则。

## 5. 目标不对齐的 enhancement（降级：接受外部 PR，不主动投入）

皮肤 / 主题 819、752、750、325；AI 与翻译 46、656、810、815、653、780、651、602；时间日期 312、531、532；LaTeX 699；网址邮箱 409；安装界面 719；工具栏淡出 205、372；标点面板 728；模糊音交互 715。

## 6. 迭代顺序

| 批次 | 内容 | 对应目标 |
|---|---|---|
| 0 | 合 #669 / #832 / #834（已完成）；核查历史 issue，不自动关闭 | 清账 |
| 1 | P0 六项 | ① |
| 2 | P1 候选窗专项（12 项同源） | ①（同时是 Android 复用 candwnd 的前置） |
| 3 | P1 引擎切分族 + P2 日语族 | ① |
| 4 | **已选 Android 路线 A** | ② |
| 5 | 在 `msime` 仓推进 Android；本仓 E1–E6 作废 | ② |
| 6 | 剪贴板同步 S1–S6 | ③ |
| 7 | P2 快捷键 / 宿主兼容 + G2 功能 | ① |

批次 5、6 可与批次 2、3 并行（不同人 / 不同分支）。

## 7. 决策记录

| # | 问题 | 结论 |
|---|---|---|
| 1 | Android 路线 | **A**：在 `msime` 仓推进；本仓 E1–E6 作废 |
| 2 | 剪贴板传输方案 | **v1 仅局域网直连**（2026-10-11 已定） |
| 3 | 同步范围 | 本轮仅剪贴板（L1）；配置、词库与短语另排 |
| 4 | E1（engine 去 Windows 化） | 因走路线 A，**本仓不再需要** |

传输方案对比、同步四层释义、免费额度与安全要求见 `docs/clipboard-sync-design.md`。

实施状态、P0 代码证据与未完成项见 [bugfix-progress.md](bugfix-progress.md)。

