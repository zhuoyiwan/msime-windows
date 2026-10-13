# zhuoyiwan 改版应用内更新

检查优先读取 `https://raw.githubusercontent.com/zhuoyiwan/msime-windows/zhuoyiwan-updates/releases.json`；静态源不可用时回退 GitHub Releases API。匿名 API 每小时限额只有 60 次，本轮实际得到 403/remaining=0，因此客户端不依赖 Token。静态清单由发布 job 在核对公开资产摘要后维护，保留精确下载 URL 和 SHA-256；Windows 只在成功检查后记录每日检查时间，失败可以立即重试。

Windows 和 Android 从 `zhuoyiwan/msime-windows` 的 GitHub Releases 读取更新，不访问上游更新源。只接受改版命名的对应平台安装包及 GitHub 资产 SHA-256；草稿、外部链接、缺少摘要、错误架构与非改版资产被忽略。Windows 比较基础版本、日期、数字修订号；Android 比较基础版本、数字修订号，r10 排在 r9 之后。

Windows 的关于页面提供检查、下载并启动安装向导、GitHub 发布页。自动检查默认关闭，开启后打开设置时每天检查一次。下载在工作线程运行，HTTPS 每次重定向检查精确 GitHub 资产主机、校验证书、限制大小并流式检查 SHA-256；失败允许重试。用户选择下载并安装后才启动安装程序。

Android 原生关于页面恢复改版更新入口，预览通道包含当前改版预发布。后台更新默认关闭，开启后每日检查并通知，安装仍走系统确认及现有同证书校验。自动更新不会覆盖用户账号、修改配对或替换为上游应用。

打包时 `Prepare-PackageFiles.ps1 -TargetVersion 0.7.1 -DisplayVersion 0.7.1-zhuoyiwan-local-20261013-r9` 保持 Windows PE 数字版本，并在设置页面写入完整改版版本，避免当前包重复提示自己是更新。Mac 从固定客户端提交完整重建原生 universal 安装包，尚未接入此更新源。

验证：前端资产筛选测试、原生请求与 HTTPS 主机拒绝测试、共享契约 fixture、设置进程编译与隔离 Server CTest。自动检查依赖设备能访问 GitHub；不宣称已在用户朋友的设备上完成联网更新验收。
