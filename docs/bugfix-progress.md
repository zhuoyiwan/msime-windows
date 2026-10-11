# Bug 核查与剪贴板改造进度

更新：2026-10-11。依据上次会话的「A → B，都你来做」执行。

## A：修复 PR 与 P0 核查

以下 PR 已合入本 fork 的 develop，并推送到 origin：

| PR | 内容 | 本地合并提交 |
|---|---|---|
| #669 | 日语 nn 转换 | 4f5bc2d4 |
| #832 | sccache 不可用时回退 MSVC | 89ca9d73 |
| #834 | 候选帧去重包含页码状态 | 9132b094 |

P0 的状态按代码证据记录；旧 issue 仍 open，不能据此断定当前版本仍有同一根因，也不能仅凭相关提交就宣布修好。

| Issue | 当前代码证据 | 结论与下一步 |
|---|---|---|
| #701 设置一直转圈 | 5aed0fcb 增加 settings_app.cpp 的 ReportWebViewStartupFailure；覆盖环境和 controller 创建的同步/异步失败，关闭 splash 并给出修复链接 | WebView2 创建失败已有处理；反馈称 Runtime 已安装，仍需在原环境复测，尚不能宣布整个 issue 修好 |
| #68 安装路径 / WebView2 缺失 | installer/msime_setup.iss 已有 AlwaysShowDirOnReadyPage=yes；Server 启动缺 Runtime 会回退 D2D；windows_webview2.cpp 失败后通过独立线程、无 owner 的提示框报告 | 路径与隐形错误提示已有处理；安装器 ReadWebView2Version 仍主要依赖注册表 pv，注册残留但文件缺失的检测仍待补齐 |
| #797 文件扫描/移动时卡顿 | 8f2c81dd 把选词学习写库移出按键线程；c280634c 启用词库 WAL | 有针对写库阻塞的改进，未做扫描压力复现；需区分 CPU/I/O 饱和、管道等待和选词学习延迟 |
| #708 CS2 聊天输入卡死 | 反馈在候选显示前就卡死，不能直接归因 window_hook.cpp | 沿 windows/src/Key → IPC → edit session 检查等待；抓游戏宿主和 Server 的挂起堆栈后再改，保留 UILess 候选路径 |
| #296 Win11 无中文/候选 | 反馈来自 v0.6.2 且信息不足；当前连接已有协议握手与失败回退 | 需当前安装包、宿主架构、初始输入模式和连接日志，暂未确认根因 |
| #421 微软日语候选消失 | ActiveLanguageProfileNotifySink 在自身 profile 停用时通知 Server、清理自身候选；候选 UI element 由自身 presenter 注册 | 沿 profile 切换与 Deactivate 生命周期复现 Edge 搜索框；未证明是 compartment 或候选 sink 污染 |

本次没有在上游发评论或关闭 issue。游戏兼容、原机环境问题仍属待复现项。

## B：剪贴板数据模型已接入

- clipboard_record.{h,cpp} 负责 v2 本地存储、v1 数组迁移、SHA-256、记录 ID 和原子替换。
- clipboard_history 的实际 Load/AddText/RemoveText/Clear 使用该模型；表情面板继续通过 Load 获取文本。
- 根对象持久化 device_id；记录带 id、text、hash、origin_device、created_at。旧条目的未知时间保留为 0，新采集条目使用 Unix 秒。
- 仍保留 50 条、每条 4000 个 UTF-16 单元；完整保留 4000 个中文字符，截断时不拆分 emoji。
- 迁移在既有命名互斥体内写回，重启与其他进程复用同一记录 ID。删除条目保留其他记录元数据；清空保留设备身份。
- 临时文件位于目标目录且名字唯一。写入、关闭或替换失败时仅清理临时文件，保留原文件。
- 限制文件为 2 MiB，拒绝损坏 JSON、非法 UTF-8 和未知版本；自动采集和删除不会覆盖无法读取的历史。
- 本格式是本地持久化格式。LAN 线协议尚未实现，应在后续统一放进 engine/contracts/，不能在两端各自复制协议定义。

## 验证

Windows 无外部依赖的测试入口：

```powershell
cmake -S server/tests/portable -B server/tests/portable/build -A x64
cmake --build server/tests/portable/build --config Release --parallel
ctest --test-dir server/tests/portable/build -C Release --output-on-failure
```

该入口运行 portable_tests 与 Windows 专用 clipboard_history_tests；后者直接编译生产用 clipboard_history.cpp，只替换数据目录提供器。完整 Server 测试目标也注册同一批测试。

已验证模型往返、Unicode 容量、异常输入、迁移 ID 稳定、增删清空、关闭采集，以及 Windows 文件共享锁阻止替换时保留旧内容。C++ 格式按 clang-format 18.1.8 检查。

本机 Release 结果：portable_tests 的 65 个用例、clipboard_history_tests 的 6 个用例全部通过，合计 71 个，0 失败。

完整 Server/安装包构建、真实游戏和旧反馈环境复测尚未执行；本机没有现成的 Server 构建树及完整依赖/词库。

## 后续

先继续 P0 复现与剩余检测修复，再实现 LAN 发现、经过身份验证的设备配对、加密传输和设置页设备管理。Android 按已选路线 A 在 msime 仓推进。本次完成数据模型基础，不代表已经能跨设备同步。
