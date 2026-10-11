# 问题分级与二改排期

- 依据：`docs/issues-triage.md`（2026-10-11 上游 issue 快照，open 161 条）
- 用途：为 fork 二改提供可直接排期的优先级与模块落点
- 注意：**工作量（F 值）是基于当前代码结构的估算，不是实测**；模块路径来自仓库实际目录

## 0. 分级方法

四个维度：

| 维度 | 取值 | 含义 |
|---|---|---|
| 严重度 S | S0 | 崩溃 / 数据丢失 / 完全不可用 / 波及其他软件 |
| | S1 | 核心输入路径失效（候选窗、切分、上屏） |
| | S2 | 特定场景失效、性能退化 |
| | S3 | 体验不一致、交互不顺手 |
| | S4 | 外观、文案、文档 |
| 影响面 A | A0 | 默认路径，全体用户 |
| | A1 | 常见宿主/场景（IDE、Office、浏览器、多屏） |
| | A2 | 特定配置或小众方案（某款游戏、某输入方案） |
| 修复成本 F | F0 | 局部改动，< 1 天 |
| | F1 | 单模块，1–3 天 |
| | F2 | 跨模块或跨进程协议，3–10 天 |
| | F3 | 需重构 / 改契约，> 10 天 |
| 二改价值 R | 高 / 中 / 低 | 与 fork 目标（插件化、自定义方案、词库、皮肤、AI）的相关度 |

优先级映射：

| 优先级 | 判定 | 处理策略 |
|---|---|---|
| P0 | S0 | 立刻修，不修则二改无意义 |
| P1 | S1 且 A0/A1 | 当前迭代主攻 |
| P2 | S1 且 A2，或 S2 | 排入后续迭代 |
| P3 | S3 | 择机，或随相关模块一并改 |
| P4 | enhancement | 按二改路线图取舍 |
| P5 | 挂起/僵尸 | 补信息或关闭，不投入开发 |

## 1. P0 阻断级（先修）

| # | 问题 | S | A | 主要落点 | F |
|---|---|---|---|---|---|
| 708 | csgo2 游戏内打字导致切屏卡死闪退 | S0 | A2 | `server/src/window/window_hook.cpp`、`windows/src/Tf` | F2 |
| 797 | 文件移动/扫描时输入法无法正常输入 | S0 | A0 | `server/src/ipc`、`server/src/session`（疑主线程阻塞） | F1 |
| 701 | 设置窗口一直转圈无法设置 | S0 | A0 | `server/src/settings/settings_app.cpp`、`ui-html/webview2/settings/` | F1 |
| 68 | 安装向导不显示路径；WebView2 缺失时候选窗一直 cloak | S0 | A0 | `installer/`、`server/src/webview2/` | F1 |
| 296 | Win11 无法打出中文、无悬浮候选框 | S0 | A0 | `windows/src/Register`、`windows/src/IPC`（需先复现） | F2 |
| 421 | 安装后系统自带日语输入法候选词消失 | S0 | A1 | `windows/src/Compartment`、`windows/src/Tf`（全局状态污染） | F1 |

## 2. P1 核心输入路径失效（当前迭代主攻）

### 2.1 候选窗显示族（同源，建议合并成一个专项）

| # | 问题 | A | 落点 |
|---|---|---|---|
| 93 | 输入一定字数后候选栏消失 | A0 | `server/src/window/candidate_presenter.cpp` |
| 618 | 单行输入框到末尾时再输入中文无候选框 | A0 | 同上 |
| 788 | 候选框会消失 | A0 | 同上 |
| 698 | 候选池选择框空白 | A0 | `ui-html/webview2/candwnd/` |
| 387 | 网页无法输出中文 | A1 | `windows/src/Edit`、`windows/src/Composition` |
| 63 | dota2 无法显示候选框 | A2 | `server/src/window/window_hook.cpp` |
| 733 | 魔兽世界加载时候选框消失 | A2 | 同上 |
| 781 | APEX 类游戏无法使用输入法 | A2 | 同上 |
| 783 | UE5.6 隐藏行内预编辑时不显示候选窗 | A2 | `windows/src/DisplayAttribute`、`server/src/window` |
| 722 | 拓展屏候选项显示 BUG | A1 | `server/src/window/ime_windows_candidate_layout.cpp` |
| 680 | 符号输入方框（字形缺失） | A1 | `ui-html/webview2/candwnd/skins/`、字体回退 |
| 772 | 高占用时候选窗不出现/延迟 | A0 | `server/src/window/candidate_presenter.cpp`（同步渲染路径） |

> 判断依据：这批问题的表现都是「窗口已建但不可见 / 被 cloak / Z-order 或帧去重丢帧」，大概率共用 `candidate_presenter` + `ime_windows_candidate_show` 的同一条生命周期；未合并 PR #834（候选帧去重纳入页面状态）正是这条线。

### 2.2 引擎切分 / 模糊音 / 辅助码族

| # | 问题 | A | 落点 |
|---|---|---|---|
| 718 | n 结尾音节 + 元音开头音节切分错误 | A0 | `engine/core/input_session_composition.cpp`、`engine/quanpin/word_lattice.cpp` |
| 803 | 输入 u 或 v（ü）导致不同分词策略 | A0 | `engine/quanpin/quanpin_query.cpp` |
| 707 | 无法正常触发拼音方案调频 | A0 | `engine/quanpin/lattice_rerank.cpp`、`engine/user_dictionary/` |
| 710 | 模糊音：双拼识别 l 是否为韵母有问题 | A1 | `engine/quanpin/fuzzy_pinyin.h`、`engine/core/fuzzy_pinyin_options.h` |
| 671 | 直接辅助码/斜杠未定字反而清空输入 | A1 | `engine/direct_helpcode/`、`engine/helpcode/` |
| 509 | 切换语言未提交已选择分词 | A1 | `engine/core/input_session_editing.cpp` |

## 3. P2 场景失效 / 性能 / 宿主兼容

### 3.1 日语族（`engine/japanese/`）

| # | 问题 | 落点 | F |
|---|---|---|---|
| 662 | 罗马字 `nn` 未按标准转为「ん」 | `engine/japanese/romaji_converter.cpp` | F0（PR #669 已写） |
| 682 | shift+R 模式无法打出长音「ー」 | 同上 + `engine/schemes/japanese_romaji_scheme.cpp` | F1 |
| 786 | 日语快捷输入长音符「ー」无法输入 | 同上 | F1 |
| 741 | 英文大写 “R” 被识别为日文 | `engine/core/`（模式判定） | F1 |

### 3.2 快捷键 / 按键拦截族

| # | 问题 | 落点 | F |
|---|---|---|---|
| 762 | Ctrl+. 被拦截，Rider 快速修复失效且无法解绑 | `windows/src/Key`、配置项 | F1 |
| 188 | 少数软件中 Ctrl+ 快捷键失效并输入字符 | `windows/src/Key`、`windows/src/Edit` | F1 |

### 3.3 宿主软件 / 编辑行为兼容

| # | 问题 | 落点 | F |
|---|---|---|---|
| 688 | Word 中无法打出不可换行空格 | `windows/src/Edit` | F1 |
| 689 | CAT 软件补齐标点时光标异常左移 | `engine/core/preedit_caret_map.h` 相关 | F1 |
| 60 | Edge Legacy 快捷键未能切换输入模式 | `windows/src/Key` | F1 |
| 818 | 大小写倒置 | `windows/src/Composition` | F1 |
| 773 | 每应用窗口不同输入法时悬浮栏状态错误 | `server/src/window/floating_toolbar_presenter.cpp` | F1 |

### 3.4 渲染细节 / 性能

| # | 问题 | 落点 | F |
|---|---|---|---|
| 505 | 任务栏在顶部时右键图标超出屏幕 | `server/src/window/tray_menu_placement.h` | F0 |
| 58 | 语言栏图标发虚（高 DPI） | `windows/src/LanguageBar` | F1 |
| 438 | 每次打开设置都要重新加载 | `server/src/settings/settings_launcher.cpp` | F1 |

## 4. P3 体验一致性（随模块一并处理）

- 候选排序/调频类：#22、#308、#464、#705
- 候选交互类：#20、#21、#468、#702、#714、#763、#779、#812、#734、#439
- 标点/上屏类：#768、#695、#748、#704
- 工具栏/面板类：#205、#372、#811、#728、#715、#805
- 设置页：#743（全局搜索）、#719（安装界面）

## 5. P4 需求：二改路线图（按契约影响排序）

### 5.1 低风险、可增量（不动契约）

| 主题 | 相关 issue |
|---|---|
| 皮肤/主题 | 819、752、750、325 |
| 快捷短语 | 826、686、758、646 |
| 符号/特殊字符 | 795、759、33、636、777、40、699 |
| 本地模式/时间日期 | 312、531、532 |
| AI 配置体验 | 46、656、810、235 |

### 5.2 中风险（改 engine 内部，不动 IPC 契约）

| 主题 | 相关 issue | 落点 |
|---|---|---|
| 输入方案扩展 | 754（五笔98）、634/660/664（五笔混输）、42（二笔）、761（注音） | `engine/schemes/` |
| 双拼/简拼优化 | 392、70、808、414、789、20、322、64 | `engine/shuangpin/`、`engine/local_modes/jianpin_query.cpp` |
| 词库管理 | 684、755、778、645、668 | `server/src/settings/dictionary_manager.cpp` |
| 日语模式扩展 | 685、72 | `engine/japanese/` |

### 5.3 高风险（改契约 / 跨进程，需先定架构）

| 主题 | 相关 issue | 影响 |
|---|---|---|
| 插件系统 | 730 | 需在 `engine/contracts/` 定义插件边界，牵动 windows/server 两侧 |
| Rime 接入 | 615 | 需引入独立引擎后端与配置映射 |
| 自定义方案导入 | 744、663 | 需方案描述格式 + 校验，涉及设置页与服务端双向 |
| 跨端同步/统计 | 678、798、712 | 需新增服务端与协议 |
| 多语言模式 | 61、294、458、461、85 | 需扩展 trilingual/方案层 |
| 网格候选 / 多行多列 | 734、21 | 需改候选布局契约与 candwnd 渲染 |

## 6. P5 清理项（不投入开发）

| 类型 | # | 建议 |
|---|---|---|
| needs-info | 42、68、296 | 补齐复现信息或关闭 |
| can't reproduce | 58、296、387 | 要求提供版本/日志，超期关闭 |
| wontfix 未关闭 | 644 | 直接关闭 |
| 未打标签 | 831、748、665 | 补标签后归入对应优先级 |

## 7. 立刻可用的增益

合并即减少未解决数，零新开发：

| PR | 内容 | 解决 |
|---|---|---|
| 669 | 日语 `nn` 单拨音转换 | #662 |
| 834 | 候选帧去重纳入页面状态 | 候选窗消失族（P1） |
| 832 | sccache 不可用时回退 MSVC | #831 |

## 8. 建议迭代顺序

| 迭代 | 内容 | 目标 |
|---|---|---|
| 迭代 0 | 合并 #669/#832/#834；关闭 #644、#296 等僵尸项 | 清账、建立基线 |
| 迭代 1 | P0 六项（#708、#797、#701、#68、#296、#421） | 消除崩溃与完全不可用 |
| 迭代 2 | P1 候选窗专项（12 项同源簇） | 解决占比最大的用户可见缺陷 |
| 迭代 3 | P1 引擎切分族 + P2 日语族 | 提升输入正确率 |
| 迭代 4 | P2 快捷键/宿主兼容族 | 覆盖 IDE 与 Office 用户 |
| 迭代 5+ | P4 路线图，按 5.1 → 5.2 → 5.3 推进 | 二改功能扩展 |

## 9. 同源簇速查

| 簇 | issue | 共用根因假设 |
|---|---|---|
| 候选窗不可见/消失 | 63、93、618、722、733、781、783、788、698、772、797 | 候选窗生命周期 / 帧去重 / cloak / Z-order |
| 日语长音与拨音 | 662、682、786、741 | `romaji_converter` 状态机 |
| 快捷键拦截 | 188、762 | `windows/src/Key` 的按键放行策略 |
| 模糊音 | 710、705、715 | `fuzzy_pinyin` 选项与排序 |
| 音节切分 | 718、803 | `input_session_composition` 切分规则 |
