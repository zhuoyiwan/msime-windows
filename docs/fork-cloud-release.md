# zhuoyiwan 三端云端构建

改版 Windows 使用 `fork-dictionary-lock.json` 固定的 `metasequoiaime/msime-dictionary` 文本源与 pypinyin 版本，由同次 run 的 dictionary job 重建。Server 测试和安装包下载同一份词库 artifact，打包阶段不再用标准 `product-lock.json` 的旧 `dict-v1.0.0` 覆盖。标准发布锁保持原约定；改版产品清单与 provenance 记录实际源提交、构建器和每个词库文件摘要。校验同时要求中译英至少 900000 条、英译中至少 65000 条及常用词词性，摘要清单必须完整。

Windows 全拼纠错补上合法切分隐藏邻键手误的路径：仅合并相邻两音节且第二音节零声母、长度至少四字母，使用既有静态邻键表的等长替换。原读法保留，候选由现有词典和整句排序决定；关闭邻键纠错或显式音节分隔不启用这条路径，不按输入串或中文句子特化。

`.github/workflows/release-three-platforms.yml` 手动触发真实三端构建，替代旧版“预上传 Windows/Android，只构建 Mac”的汇集流程。只允许用户 fork，不合入 main，也不调用标准 Release 工作流。

输入固定的 `zhuoyiwan/msime` 完整源码 SHA、指向本次 Windows 工作流提交的草稿 tag 和 Windows 修订号。Windows 构建 x64/x86 TSF、完整 Server 测试和页面测试，再打未签名 Inno 包；Android 构建 full ARM64，使用原正式证书签名并核对指纹；Mac 完整重建 universal 原生 DMG，检查本机自签名安装路径。

发布依赖 API 28/35 模拟器上的 client-core 全套测试、原生装包验收，以及本轮长句/多错输入、软件主题与词库加载、语音 final 提交测试。上游设备工作流既有 FuzzyPinyinDeviceSmoke 跳过项保持并明确打印，其余失败阻止发布。模拟器语音 fixture 不代表真实手机麦克风和 ASR 服务端到端耗时。

三个安装包必须来自同一次 Actions run，固定来源、大小和 SHA-256 写入 `release-provenance.json`。上传后逐个核对 GitHub digest，再把草稿发布成 prerelease。失败重跑只接受同名且字节摘要一致的草稿资产；不覆盖公开 Release 或不同来源资产。Android secrets 只存于用户 fork，证书指纹固定为 `2cc60b29966625bc9834128a71bd1d7702d3137f7bd791d5093b9b68197a3385`。

草稿先由 `gh release view --json databaseId` 解析 Release ID，再通过 `/releases/{id}` 读取资产。GitHub 的 `/releases/tags/{tag}` 在草稿仍存在且有读取权限时也会返回 404，不能用它校验发布前资产；公开后的 tag 查询可用。

Windows 未签名，uiAccess 关闭；Mac 没有 Developer ID/notarization，使用包内本机安装脚本。普通用户 Mac 登录会话输入源、真实手机 ASR、朋友热点连接仍需实机验收。

## 2026-10-13 r6 发布记录

[三端预发布](https://github.com/zhuoyiwan/msime-windows/releases/tag/zhuoyiwan-20261013-r6)由 [Actions 38300269632](https://github.com/zhuoyiwan/msime-windows/actions/runs/38300269632) 完整构建并公开，11 个 job 全部成功。Windows 与工作流固定在 `8f1f56150121f34baee80a5f21ecdb75cfc30fc2`，Android/Mac 固定在 `6c665fe7f613244e8ed5d40c743483803a279c55`；没有复用旧二进制。

Server 出货词库 CTest 3/3、设置页 149 测试、Linux Rust 引擎 1830 测试及 Clippy 通过。API 28/35 client-core 各 1305 测试通过，装包输入与本轮三个专项通过；API 35 的标准偏好、图片传输、邮箱、键盘高度和物理键盘候选等通过。既有模糊拼音跳过项不记作通过。Mac universal 打包、四项 CTest、本机签名安装及 IMK 注册查询入口通过，普通用户登录会话仍未验收。

公开后下载全部七个资产，逐项核对 GitHub digest、`SHA256SUMS.txt`、来源提交及同一次 run。实际 APK 再经 apksigner/aapt 核对证书、ARM64、versionCode 3006 和 `0.3.1-zhuoyiwan-r6`。三个安装包的 SHA-256：

- Windows r9：`473a701527fdf506bd844495a244db8c24f30482b3db0b4e072a19650c5b0138`。
- Android r6：`cf2873237aeee04449d3baffface681235604f172b72400be3c0d9ff56d4b109`。
- Mac 0.52.0 universal：`3963bef6393e6aa597d50da5c3c5fba6fa6e57c0c62e9bf765c37116a52c7d9a`。

本机 Windows quick 仍失败，Rust workspace 编译与默认配置契约通过；既有契约失败及 Git Bash 路径、POSIX 工具、SQLite 文件占用等限制保留记录，不增加 known-failures 豁免，也不将 quick 写成全绿。
