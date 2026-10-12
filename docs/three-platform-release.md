# zhuoyiwan 三端改版发布

Windows 使用本仓的 TSF DLL、Server 和 Inno Setup；Android 与 macOS 原生宿主的源码位于 [zhuoyiwan/msime](https://github.com/zhuoyiwan/msime)，按完整 commit SHA 固定。Windows 工程不能通过修改 CMake 配置生成 macOS 输入法。

三端安装包统一放在本仓同一个 Release。`release-three-platforms.yml` 手动运行，参数是 Mac/Android fork 源码完整 SHA 和草稿 Release tag（格式 `zhuoyiwan-YYYYMMDD-rN`）。先把已交付的 Windows EXE、同证书 ARM64 Android APK 和 `release-provenance.json` 上传到草稿；工作流核对摘要后用 GitHub 托管的 macOS runner 编译 universal DMG、验证核心资源及本机签名路径，再补齐三端 `SHA256SUMS.txt`。成功后核对资产并将草稿公开。不会触发 `main` 的自动发版、写入上游 Release 或上游 Homebrew tap。

`release-provenance.json` 包含 `macos_source_commit`、`windows` 和 `android`；后两项记录 `asset`、`sha256`、`source_commit`。发布中的固定 SHA 对应用户实际安装的改版源码；不上传私钥、密码或本机缓存。

Mac 无 Apple Developer ID 签名、公证，DMG 内附本机安装脚本和说明；脚本在当前用户钥匙串生成代码签名证书，再签名并启动设置应用。CI 的签名检查不等于真实 Mac 输入源注册和中文输入验收。Mac 尚未移植本改版的局域网剪贴板、去账号模式及本轮前缀补全。

Android r4 的完整设备验收来自 API 35；API 28 手机号曾实际补全成功，但跨编辑器/旋转套件未完整通过。QQ 游戏小窗候选需真机复测。Windows r6 未使用受系统信任的证书，安装包保留 `unsigned` 标识。
