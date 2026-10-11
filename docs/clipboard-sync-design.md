# 多设备剪贴板共享 · 方案说明与决策

- 关联：`docs/issues-grading.md`（分级 v2）、上游 issue #678 / #712 / #811 / #798
- 本文件记录决策，并给出 v1 局域网方案的具体设计

## 0. 决策状态

| # | 问题 | 结论 |
|---|---|---|
| 1 | Android 路线 | **A**：在 `msime` 仓（Rust/TS）推进 Android 宿主 |
| 2 | 传输方案 | **已定：v1 只做局域网直连** —— 不用 WebDAV、不用服务器、不用 Cloudflare |
| 3 | 同步范围 | 建议 v1 仅 L1 剪贴板（见 §1 的四层释义） |
| 4 | E1（engine 去 Windows 化） | 因走路线 A，**本仓不再需要** |

## 1. 同步范围释义（四层）

| 层 | 内容 | 落盘位置 | 同步难度 |
|---|---|---|---|
| L1 剪贴板历史 | 用户复制的文本 | `<IME 数据目录>/clipboard_history.json` | 低 |
| L2 用户词库 / 学习记录 | 被记住的词、调频、固定候选位 | `msime_user.db`、`user_data/dictionaries/<content ID>/{msime.db,english.db}` | **高**（双向合并冲突） |
| L3 配置 | 设置页全部选项 | 用户 `config.toml` | 中（含机器相关项） |
| L4 快捷短语 | 自定义短语 | 用户数据 | 低-中 |

上游 #678 的原话是「配置参数、用户词库等手动处理麻烦」，即用户想要的是 **L2 + L3**。本方案 v1 只做 **L1**，L2/L3/L4 留到后续。

### L3 的硬约束（已核实）

`server/src/config/credential_protection.h` 明确：`config.toml` 里的凭证以 **DPAPI（当前用户范围）** 密文落盘，「只有同一个 Windows 账户能解开」。因此凭证密文**无法跨设备同步**，将来做 L3 时必须排除凭证键或另设同步密钥。

## 2. 初始定位（记录实现前的状态，当前进度见 §6）

### 2.1 存储

| 项 | 事实 |
|---|---|
| 落盘文件 | `<IME 数据目录>/clipboard_history.json`（`ClipboardHistory::StorePath()`，`clipboard_history.cpp:520`） |
| 格式 | JSON 字符串数组，UTF-8 |
| 上限 | 50 条 / 每条 4000 字符（`kMaxItems` / `kMaxChars`） |
| 写入 | `std::ofstream` + `trunc`，**非原子**（无 temp + rename），`clipboard_history.cpp:280` |
| 并发保护 | 命名互斥体 `Local\MetasequoiaIme.ClipboardHistory` |
| 开关 | `config.toml` 的 `[utility] clipboard_history`，互斥体 `Local\MetasequoiaIme.ConfigFile` |

### 2.2 采集

- 消息专用窗口 `MetasequoiaIme.ClipboardListener` + `AddClipboardFormatListener` + `GetClipboardSequenceNumber`
- 定时器去抖 80ms（`kCaptureDebounceMs`）
- `ClipboardMonitor::{Start,Stop,Sync,IsRunning}` 跑独立线程

### 2.3 消费者

| 位置 | 用途 |
|---|---|
| `server/src/emoji-panel/EmojiPanel.cpp` | 剪贴板面板（**目前嵌在 emoji 面板里**，正对应 issue #811 的诉求） |
| `server/src/config/ime_config_general.cpp:522` | 开关变更时清空 |
| `server/src/config/ime_config.cpp:594` | 启动时同步状态 |
| `server/src/main.cpp:211` | 退出时停止监听 |

### 2.4 结论：要同步必须先改数据模型

现有条目**只有文本本身**，没有 id / 时间戳 / 来源设备 / 哈希，无法做去重与冲突判定。

## 3. v1 局域网方案设计

### 3.1 总体结构

```
[Windows：server 进程]                    [Android：msime 仓]
   ClipboardMonitor（采集）                  ClipboardService（采集）
          │                                        │
          └────────► LAN Sync 模块 ◄───────────────┘
                  发现 / 配对 / 加密 / 传输
```

无服务器、无域名、无证书、无云端。第一版采用手动私网 IPv4；自动发现留到后续。

### 3.2 发现（Discovery）

| 方案 | 说明 |
|---|---|
| mDNS / DNS-SD（首选） | Windows 10 1703+ 有原生 API（`windns.h` 的 `DnsServiceRegister` / `DnsServiceBrowse`）；Android 用 `NsdManager` |
| UDP 组播（兜底） | 固定组播地址 + 端口，兼容不支持 mDNS 的环境 |
| 手动填 IP（最后兜底） | 组播发现不可用但单播仍可达时使用；不能绕过 AP 隔离 |

### 3.3 配对与加密

- 首次配对：传递完整随机配对码（256 位秘密），五分钟过期、单次使用；不采用短 PIN 或自制握手
- 传输：TLS 1.3 external PSK + ECDHE，TLS_AES_128_GCM_SHA256，禁用早期数据和会话恢复
- Windows 长期凭证用当前用户 DPAPI 加密，撤销配对后拒绝后续认证；细节见 [clipboard_lan.md](../engine/contracts/clipboard_lan.md)
- **局域网同样必须加密**：公共 WiFi / 公司网络同网段可能有其他人

### 3.4 传输

- TCP（简单可靠）或 QUIC；端口固定
- 安装器需添加防火墙入站规则（Windows 默认拦截入站）

### 3.5 数据模型（新）

由纯字符串数组升级为带元数据的记录，并保留旧格式迁移：

```json
{
  "version": 2,
  "device_id": "local-device-id",
  "items": [
    {
      "id": "uuid",
      "text": "...",
      "hash": "sha256",
      "origin_device": "device-uuid",
      "created_at": 1699999999
    }
  ]
}
```

- 读到 v1 的纯数组时自动迁移
- 本地存储根对象的 `device_id` 保持设备身份，清空历史时保留；它不是传输协议字段。
- 旧条目的未知 `created_at` 为 0；新采集记录使用 Unix 秒。容量保持 50 条、每条 4000 个 UTF-16 单元。
- `hash` 用于去重；`origin_device` 用于防止回环广播
- 同步写入本地历史的条目**不再触发对外广播**，也不自动改写系统剪贴板（仅在面板中出现）
- 接收校验与原子导入已实现为 ClipboardHistory::ImportRecord；配对认证由后续传输层负责。
- LAN 事件格式、边界和跨语言固定向量见 [clipboard_sync.md](../engine/contracts/clipboard_sync.md)。本地 JSON 的版本 2 与事件协议版本 1 分别演进。

### 3.6 代码落点

| 任务 | 位置 |
|---|---|
| 数据模型改造 + v1 迁移 | `server/src/clipboard/clipboard_history.{h,cpp}` |
| 写入原子化（temp + rename） | 同上（引入网络写入后必须做） |
| LAN 同步模块（发现 / 配对 / 加密 / 传输） | 新增 `server/src/sync/` |
| 设备列表与配对 UI | `ui-html/webview2/settings/` |
| 剪贴板面板 | `server/src/emoji-panel/EmojiPanel.cpp`（可参照 #811 独立出来） |
| 防火墙规则 | `installer/` |
| Android 端 | `msime` 仓 |

## 4. 已知限制（局域网方案的真实代价）

1. **AP 隔离**：多数公共 / 访客 WiFi 禁止设备互访，发现与连接都会失败；手动 IP 无法绕过隔离，需要更换网络或调整路由器设置。
2. **跨网段**：2.4G 与 5G 访客网络若互相隔离，组播可能不通。
3. **Windows 防火墙**：默认拦截入站，安装器不加规则用户会直接看到连接失败。
4. **离开同一网络即不可用** —— 这是 v1 的主动取舍，不是缺陷。
5. **写入可靠性**：本轮已接入同目录唯一临时文件 + rename，替换失败保留旧文件；原子替换不承诺断电时的持久化。

## 5. 后续（不在 v1 范围）

- 跨网络：WebDAV，或自建服务器（`msime-cloud`）+ Cloudflare Tunnel 入口
- L2 词库 / L3 配置 / L4 快捷短语同步（L3 必须排除 DPAPI 加密的凭证）
- 统计同步（#798）

## 6. 下一步

1. 数据模型改造 + v1 迁移 + 写入原子化（已接入并验证；见 [bugfix-progress.md](bugfix-progress.md)）
2. LAN 手动 IP、配对、TLS 加密及双向传输（已实现并通过回环/服务测试）；自动发现待实现
3. 设置页设备管理 UI（已实现）
4. Android 侧对接（`msime` 仓）

Windows 已接上手动私网 IPv4 配对、加密收发、历史导入和设备管理。新采集记录才进入易失发送队列，关闭或退出即清空；不回填旧历史、不自动设置系统剪贴板。Android 对接、自动发现和真实双机安装验证仍待完成。

## 附录 A：传输方案对比（备查，说明为何选局域网）

| 方案 | 需要服务器 | 费用 | 跨网络 | 实时性 | 实现成本 | 主要缺点 |
|---|---|---|---|---|---|---|
| **局域网直连（已选）** | 否 | **完全免费** | 否 | 高 | 中 | 离开同一网络即断 |
| WebDAV（用户自带网盘） | 否 | 免费额度可用 | 是 | 低（轮询） | 低 | 依赖第三方 |
| 自建 `msime-cloud` 中转 | 是 | 服务器要花钱 | 是 | 高 | 高 | 运维 + 账号体系 |
| Cloudflare Workers + Durable Objects | 否 | 免费额度（10 万请求/天、DO 13,000 GB-s/天；Free 计划 DO 仅 SQLite 后端） | 是 | 高 | 中 | `workers.dev` 在中国大陆常不可达，须绑自定义域名 |
| Cloudflare Tunnel | 否（入口） | 免费 | 是 | 高 | 低 | 仍依赖自有服务器 |
| Tailscale / WireGuard | 否 | 个人免费 | 是 | 高 | 中 | 用户需装额外客户端 |
| Syncthing | 否 | 完全免费开源 | 是 | 中 | 极低 | 体验割裂，依赖用户自装 |
| 系统云剪贴板 | 否 | 免费 | 是 | 高 | — | Android 无对等开放 API，不可用 |
