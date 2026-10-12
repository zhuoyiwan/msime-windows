# zhuoyiwan 三端云端构建

`.github/workflows/release-three-platforms.yml` 手动触发真实三端构建，替代旧版“预上传 Windows/Android，只构建 Mac”的汇集流程。只允许用户 fork，不合入 main，也不调用标准 Release 工作流。

输入固定的 `zhuoyiwan/msime` 完整源码 SHA、指向本次 Windows 工作流提交的草稿 tag 和 Windows 修订号。Windows 构建 x64/x86 TSF、完整 Server 测试和页面测试，再打未签名 Inno 包；Android 构建 full ARM64，使用原正式证书签名并核对指纹；Mac 完整重建 universal 原生 DMG，检查本机自签名安装路径。

发布依赖 API 28/35 模拟器上的 client-core 全套测试、原生装包验收，以及本轮长句/多错输入、软件主题与词库加载、语音 final 提交测试。上游设备工作流既有 FuzzyPinyinDeviceSmoke 跳过项保持并明确打印，其余失败阻止发布。模拟器语音 fixture 不代表真实手机麦克风和 ASR 服务端到端耗时。

三个安装包必须来自同一次 Actions run，固定来源、大小和 SHA-256 写入 `release-provenance.json`。上传后逐个核对 GitHub digest，再把草稿发布成 prerelease。失败重跑只接受同名且字节摘要一致的草稿资产；不覆盖公开 Release 或不同来源资产。Android secrets 只存于用户 fork，证书指纹固定为 `2cc60b29966625bc9834128a71bd1d7702d3137f7bd791d5093b9b68197a3385`。

Windows 未签名，uiAccess 关闭；Mac 没有 Developer ID/notarization，使用包内本机安装脚本。普通用户 Mac 登录会话输入源、真实手机 ASR、朋友热点连接仍需实机验收。
