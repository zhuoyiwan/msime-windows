[CmdletBinding()]
param([Parameter(Mandatory)][string]$ReleaseTag, [int]$Revision = 9)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($ReleaseTag -notmatch '^zhuoyiwan-([0-9]{8})-r[0-9]+$' -or $Revision -lt 1) {
    throw '改版发布标识不合法'
}
$date = $Matches[1]
$version = (Get-Content version.txt -Raw).Trim()
$display = "$version-zhuoyiwan-local-$date-r$Revision"
python scripts/product_lock.py fetch-dictionaries --staging-root .
if ($LASTEXITCODE) { throw '词库下载或校验失败' }
./scripts/build-language-model.ps1
./scripts/fetch-neural-model.ps1
./scripts/ci/install-inno-language.ps1
./installer/Prepare-PackageFiles.ps1 -TargetVersion $version -DisplayVersion $display -RepoRoot $env:GITHUB_WORKSPACE
python scripts/product_lock.py manifest --windows-commit $env:GITHUB_SHA --output installer/app_data/product-manifest.json
if ($LASTEXITCODE) { throw '产品来源清单失败' }
./installer/Compile-Installer.ps1 -Unsigned
$asset = "MetasequoiaIME_Setup_v$display-unsigned.exe"
New-Item -ItemType Directory -Force dist | Out-Null
Copy-Item -LiteralPath "installer/Output/MetasequoiaIME_Setup_v$version.exe" -Destination "dist/$asset"
python scripts/ci/fork-release-metadata.py record --platform windows --directory dist --source $env:GITHUB_SHA --asset $asset --version $display
if ($LASTEXITCODE) { throw 'Windows 产物来源记录失败' }
