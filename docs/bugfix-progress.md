# Bug 核查与剪贴板改造进度

更新：2026-10-11。依据上次会话的「A → B，都你来做」执行。

## 2026-10-12：系统剪贴板与安卓使用体验

- 修复仅入历史、普通粘贴仍是旧文字：已认证的新事件进入 Windows 系统剪贴板；监听线程写入并记序号，避免再次发布。排队等待剪贴板锁期间若出现更新的本机复制，保留本机内容。
- 同文重新复制生成新事件，历史仍只保留一条同文；接收服务记住有限的旧事件 ID，旧重传不会覆盖新事件。
- 安卓改版移除可达账号/登录页面、登录 SDK、回调和会话 provider；主页提供设备互通、本机历史与备份。同步监听独立于历史页面，由可停止的前台连接服务保持运行，系统剪贴板写入触发最近复制提示且不回传。
- 工具栏一键打开历史，精简模式也保留入口；数字层按 123 / 456 / 789 排列，0 和小数点放在底行。安卓使用此前证书签名，版本 0.3.1-zhuoyiwan-r2（3002）。
- Windows 构建、全部 CTest 与 clang-format 18.1.8 检查通过。Android ARM64 编译/lint、签名与 16 KB 对齐检查通过，DEX 中无已移除的账号组件与 Google 登录 SDK。
- API 35 模拟器与 Windows 生产 TLS 实现的双向收发、Android 普通粘贴/最近复制、一键历史、精简工具栏、数字位置和点按通过；原中文输入、繁体、删除、密码与组字回收回归通过。Windows 隔离系统剪贴板烟测因创建 window station 被执行环境拒绝未运行；安装后桌面粘贴及真实手机省电策略仍需用户实测。
- 安装包生成到 installer/Output；没有运行安装器或改动真实安装配置，大缓存与产物保持在 F 盘。

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
| #68 安装路径 / WebView2 缺失 | 已有路径提示、D2D 回退和可见失败提示；本轮为 ReadWebView2Version 增加实际文件检查与跨视图/用户级回退 | 注册残留但文件缺失的检测缺口已修，并通过生产函数回归探针；完整安装包和原反馈机器仍待复测 |
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

测试入口（本轮新增的 TLS 和 Windows 配对目标需要 OpenSSL 3.x 与 nlohmann/json 开发头；标准 vcpkg 环境可提供）：

```powershell
cmake -S server/tests/portable -B server/tests/portable/build -A x64 "-DOPENSSL_ROOT_DIR=C:/path/to/openssl" "-DNLOHMANN_JSON_INCLUDE_DIR=C:/path/to/json/include"
cmake --build server/tests/portable/build --config Release --parallel
ctest --test-dir server/tests/portable/build -C Release --output-on-failure
```

该入口运行 portable_tests 与 Windows 专用 clipboard_history_tests；后者直接编译生产用 clipboard_history.cpp，只替换数据目录提供器。完整 Server 测试目标也注册同一批测试。

已验证模型往返、Unicode 容量、异常输入、迁移 ID 稳定、增删清空、关闭采集，以及 Windows 文件共享锁阻止替换时保留旧内容。C++ 格式按 clang-format 18.1.8 检查。

首次改造 Release 结果为 71 个用例通过；本轮加入 LAN 数据处理后为 portable_tests 72 个、clipboard_history_tests 9 个，合计 81 个，0 失败。

## 本轮继续：检测缺口与 LAN 数据处理

- 安装器检查注册表版本及对应 msedgewebview2.exe 文件；残留键、无效版本或文件缺失继续查找其他机器级/用户级安装位置。
- installer/tests/webview2-runtime.ps1 抽取生产 Pascal 函数，使用临时目录文件和模拟注册表读取运行探针，不改系统 WebView2。覆盖缺失文件、残留键、备用安装和无效版本。已登记 Windows CI。
- 本机通过临时目录中解包的 Inno Setup 6.0.5 编译器运行这一纯 Pascal 探针；没有运行编译器安装程序。完整安装器要求 6.6+，CI 用 runner 自带的编译器验证同一探针，本轮未编译完整安装包。
- 唯一事件定义在 engine/contracts/clipboard_sync.h；本地记录复用该类型与容量，编码/解码不依赖 Windows。明确验证长度、版本、UTF-8、记录身份及跨语言固定向量。
- 新增 server/src/sync/clipboard_receive_policy.*，验证来源与已认证设备匹配、SHA-256 一致，拒绝冲突/反射/重复，按接收顺序保持最多 50 条。
- ClipboardHistory::ImportRecord 已接入真实历史文件，保留远端元数据、尊重关闭采集、保持原子写入；损坏历史或写入失败不覆盖旧文件。
- 只有新的本机采集记录可供后续发送；迁移来的旧历史和接收记录不会再广播。
- 本轮没有启用网络监听。加密连接与设备配对仍是接入上述 API 的前置；传输层还需负责会话重放防护，历史去重不能替代它。

完整 Server/安装包构建、真实游戏和旧反馈环境复测尚未执行；本机没有现成的 Server 构建树及完整依赖/词库。

## 本轮继续：局域网加密传输

- Windows 已接入手动私网 IPv4、单次五分钟配对码、TLS 1.3 external PSK + ECDHE、双向批次收发及真实历史导入。协议和跨语言约定见 `engine/contracts/clipboard_lan.{h,md}`。
- 配对密钥由 OpenSSL CSPRNG 生成，凭证文件整体用当前用户 DPAPI 加密并原子替换；过期、取消、错误身份、撤销及损坏文件均拒绝认证。新配对最多八台。
- 设置页加入 opt-in 开关、生成/取消配对码、填写私网 IP 与完整配对码、刷新/撤销设备。设备列表表示保存的信任，不冒充实时在线状态。出厂模板、工作配置及两端读写映射均已同步。
- Server 统一负责采集，设置进程不再启动重复监听，避免抢先写入而漏发。只有成功的新本机采集进入队列；导入、迁移及启动前历史不广播。复制包含配对码前缀的文本不会保存或分享。
- 单设备队列最多 50 条，成功确认后删除已发送 ID，保留传输期间的新采集；关闭同步会跨进程通知清空队列，退出也清空。该队列不持久化，不保证长期离线补发。
- 网络只接受私网 IPv4，TLS 固定 1.3 / TLS_AES_128_GCM_SHA256，要求 ECDHE，关闭 0-RTT、tickets 和恢复缓存。网络 I/O 有整体三秒期限和停止轮询；存储操作仍受既有互斥体超时控制。
- 安装器加程序级私有网络/本地子网入站规则，卸载时移除；数据目录迁移同时保留历史和加密配对文件。只编译了防火墙函数探针，未运行安装器或改变本机规则。
- 本机临时目录构建了校验 SHA256 后的 OpenSSL 3.5.9，使用真实 TCP/TLS 做双端测试。Release 共 **96 个 C++ 用例**通过（72 portable、9 history、8 TLS、7 pairing/service）；涵盖错误密钥、未知 key、冒充设备、明文绕过、停止中断、过期/撤销、DPAPI、真实服务采集→发送→远端导入和不回填旧历史。
- 设置页 TypeScript 检查、Vite 生产构建及 **129 个前端用例**通过；**68 个共享 WebView 契约 fixture**通过。本机缺少 `python3` 命令别名，采用同样的 `python sync-contracts.py --check`、`pnpm exec tsc --noEmit` 与 `pnpm exec vite build` 完成构建步骤。
- Linux ASan/UBSan job 已加入真实 TLS 测试和 libssl-dev 依赖；本机未跑 Linux sanitizer。仍未构建完整 Server/设置 EXE/安装包，也未进行真实双机安装、Android 互通或不同 DPI 下的设置页目视验证。

## 本地打包检查：用户数据保留

- 修正安装器的覆盖安装保留名单：剪贴板历史和 DPAPI 配对凭证升级时保留、卸载时删除，与数据目录迁移名单一致。否则覆盖安装会清空历史，并因设备身份变化使原配对失效。
- `installer/tests/preserve-user-data.ps1` 加入两项回归约束；保留/迁移名单检查、完整/轻量打包契约和构建失败中止检查均通过。
- 使用静态解包的 Inno Setup 6.7.3 编译器通过 WebView2 检测探针；没有安装编译器或更改系统 WebView2。
- 为 `msime_clipboard_lan` 补上 MSVC `/utf-8`：完整构建发现该目标早于目录级编译选项创建，按本机代码页解析中文注释会吞掉后续命名空间声明；独立测试目标已有此选项，所以之前没有暴露。
- Visual Studio 2022 完整构建 x86、x64 TSF DLL；两套各 12 项 CTest 全部通过。Server、设置程序及全部面板也已完整构建，Server 的 **666 个用例**、语音控制测试及 **68 个共享契约 fixture** 在隔离数据目录通过。
- 本地从干净的 `msime-dictionary` 提交 `4909582a8ce345c23809750e675e3d9e5f9516df` 构建完整词库；语言模型和神经模型通过各自锁文件摘要校验。构建依赖使用 vcpkg 锁定基线，TLS 用 OpenSSL 3.6.4。
- Inno Setup 6.7.3 编译完整未签名测试包 `MetasequoiaIME_Setup_v0.7.1-zhuoyiwan-local-20261011-unsigned.exe`，归档完整性检查通过。没有安装、注册 TSF、信任测试证书或修改防火墙；实际双机安装、Android 互通和 DPI 目视验证仍未执行。
- 设置页“关于”和安装器名称标明“zhuoyiwan 改版”，说明基于开源水杉输入法二次开发与维护，保留原项目名称和许可链接；前端 TypeScript 检查、生产构建通过，实际解包确认关于页和安装/卸载名称均包含标识，并生成 SHA256 校验文件。

## 后续

继续真实双机安装与完整构建验证，加入自动发现，并在已选路线 A 的 msime 仓实现 Android 宿主及此 TLS profile 对接。P0 真实场景复现仍需继续，不把相关基础改动当成所有反馈均已修复。

## 安装后修正：Server 无法启动与状态图标模糊

- 用户安装首个未签名测试包后，英文可输入，但中文无候选且无法上屏。实际检查发现仅设置程序和看门狗在运行；手动启动 Server 报“从服务器返回了一个参照”。根因是未签名 Server 的清单仍请求 `uiAccess=true`，Windows 拒绝启动，之前归档校验和单元测试没有覆盖这个运行期条件。
- `Compile-Installer.ps1` 增加 `-Unsigned` 并自动识别未签名 Server，在暂存副本中注入 `uiAccess=false`、读回验证后再编译；发布流程明确传入签名模式。签名构建保留原清单，拒绝为未签名模式修改已签名的二进制。构建失败回归加入 false 清单、错误读回拒绝和签名保护检查，通过。
- 已从仓外修正副本实际启动 Server，使用已安装的数据目录；用户确认候选恢复正常。安装目录写入权限不足，未替换已安装的 EXE，临时进程用于本次恢复；重启后的持久修复需覆盖安装新包。
- “中 / 英”图标改为逐尺寸绘制、直线笔画对齐目标像素，斜线只对边缘做抗锯齿；覆盖 16、20、24、28、32、36、40、44、48、64 像素，包含浅色/深色资源。水杉标志的小尺寸保留绿色边框、深色底和白色折线，清除原有纹理缩小产生的模糊；64 像素及以上的原品牌图像保持不变。生成入口为 `windows/scripts/generate-taskbar-icons.py`（Pillow），`--check` 校验产物一致性。
- x86、x64 TSF 重新构建，两套各 12 项测试通过；实际从两套 DLL 加载 90 个图标资源，尺寸和 32 位色深均正确。静态预览检查常用 DPI 尺寸；任务栏最终显示需覆盖安装、重启宿主后目视确认，未宣称已完成所有显示器的系统缩放实测。
- 新完整测试包为 `MetasequoiaIME_Setup_v0.7.1-zhuoyiwan-local-20261011-r2-unsigned.exe`，包含启动清单修正和新图标；归档完整性通过，实际解包确认 Server 请求 `asInvoker` / `uiAccess=false`，Server 和两套 TSF 的 SHA256 与本轮暂存产物一致，附带 SHA256 校验文件。仍不具备向管理员权限应用提供候选浮层的 uiAccess 能力。

## 按设备身份自动重连、双向配对与扫码

- 配对以设备 ID、key ID 和密钥验证，保存的 IP 仅作连接提示。新增 UDP 49877 的 HMAC-SHA256 发现协议，向活跃私网接口广播并探测上次地址；严格校验新鲜 nonce、身份、长度和摘要，拒绝未请求/重放响应。发现不创建信任，连接仍经 TLS 1.3 PSK + ECDHE 验证双方身份，成功交换后才持久化最新地址。契约及跨平台固定 HMAC 向量记在 `engine/contracts/clipboard_lan.md`。
- 同一次配对码连接成功后，双方自动保存对方；设置页可见时自动刷新设备列表。发现到的新 IP 不需要重新配对；旧地址的未完成探测不会挡住对新地址的反向验证。撤销停止对应设备的授权；关闭共享会关闭监听并清理临时地址和未发送队列。
- 设置页改为“让另一台设备连接本机 / 连接另一台设备”两组，按钮按用途排列；“配对并连接”采用随明暗主题反转的中性色，IP 移入可选的高级项。生成五分钟单次二维码，支持摄像头和本地图片扫码，也保留手动配对码。扫码只填表并显示设备 ID，用户确认后才配对；二维码不触发 URL 跳转。关闭、取消、切页、窗口隐藏和延迟权限返回均会释放摄像头；只为本地设置页来源弹出摄像头确认，不持久保存授权。
- 安装器增加程序级 UDP 49877 入站规则，沿用私有网络、本地子网限制；升级/卸载同步移除两条剪贴板规则。新增二维码依赖的许可随第三方声明分发。
- Server / 设置 EXE 完整构建通过，隔离词库与配置运行 **671 个 Server 用例**及语音控制测试、**68 个共享契约 fixture**。真实 UDP/TLS 测试覆盖双向发现、双端配对、无初始 IP 的连接、远端更换 IP 和 TCP 端口后继续导入文本、伪造/重放拒绝与撤销。前端 TypeScript、生产构建和 **143 个用例**通过，包括最长设备 ID 的二维码生成→实际解码、延迟相机/图片清理、取消邀请响应与设备列表自动刷新；clang-format 18.1.8 格式检查通过。
- 新完整未签名本地包为 `MetasequoiaIME_Setup_v0.7.1-zhuoyiwan-local-20261011-r3-unsigned.exe`，保留 r2 的启动清单和图标修正。归档完整性及实际解包验证通过，13 个原生 EXE/DLL、两套 TSF 与 77 个设置资源的 SHA256 均与暂存产物一致（关于页使用打包时注入的版本）；Server 清单为 `asInvoker` / `uiAccess=false`，没有混入测试程序、PDB 或用户配对凭证。安装包 SHA256 为 `a1b6d5aaabdc44122fe6701700c9d35e9e69c8f41fc764c7f7be065a4aaf274f`，附校验文件。打包验证只解包，不安装、注册 TSF 或修改本机防火墙。实际分离双机局域网、摄像头硬件、Android 互通和不同 DPI 的界面目视检查仍未完成；本轮自动化环境没有可用浏览器，不能把响应式 CSS 与测试通过记作实测显示效果。
