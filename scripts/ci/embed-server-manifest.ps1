# Embed the Server's uiAccess manifest before the binary is uploaded or signed.
# Authenticode signs the exact PE bytes, so this must run before sign-binaries.ps1.
[CmdletBinding()]
param(
    [string]$BuildDir = 'server/build-release/bin/Release',
    [string]$ManifestPath = 'server/MetasequoiaImeServer.manifest',
    [string]$ManifestTool,
    [ValidateSet('true', 'false')][string]$UiAccess = 'true'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildRoot = (Resolve-Path (Join-Path $repoRoot $BuildDir)).Path
$binary = Join-Path $buildRoot 'MetasequoiaImeServer.exe'
$manifest = (Resolve-Path (Join-Path $repoRoot $ManifestPath)).Path

if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Server binary not found: $binary"
}

if ([string]::IsNullOrWhiteSpace($ManifestTool)) {
    $ManifestTool = (Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\mt.exe' |
        Sort-Object FullName -Descending | Select-Object -First 1).FullName
}

$manifestCommand = if ([string]::IsNullOrWhiteSpace($ManifestTool)) {
    $null
}
else {
    Get-Command -Name $ManifestTool -ErrorAction SilentlyContinue
}
if ([string]::IsNullOrWhiteSpace($ManifestTool) -or
    (-not $manifestCommand -and -not (Test-Path -LiteralPath $ManifestTool -PathType Leaf))) {
    throw 'mt.exe not found in the Windows SDK.'
}

# Read the resource back through mt.exe so a successful process exit cannot hide a
# wrong resource target or a malformed manifest. The temporary file never enters the
# package and is removed even when validation fails.
$probe = Join-Path ([IO.Path]::GetTempPath()) ("msime-server-manifest-" + [Guid]::NewGuid() + '.xml')
$unsignedManifest = $null
try {
    if ($UiAccess -eq 'false') {
        if ((Get-AuthenticodeSignature -LiteralPath $binary).Status -ne 'NotSigned') {
            throw 'Refusing to change a signed Server binary for an unsigned package.'
        }
        # Windows refuses to launch an unsigned executable requesting uiAccess.
        # Change only the staged payload; keep the source manifest for signed builds.
        [xml]$document = Get-Content -LiteralPath $manifest -Raw
        $levels = $document.SelectNodes('//*[local-name()="requestedExecutionLevel"]')
        if ($levels.Count -ne 1) { throw 'Expected one Server requestedExecutionLevel.' }
        $levels[0].SetAttribute('uiAccess', 'false')
        $unsignedManifest = Join-Path ([IO.Path]::GetTempPath()) ("msime-unsigned-manifest-" + [Guid]::NewGuid() + '.xml')
        $document.Save($unsignedManifest)
        $manifest = $unsignedManifest
    }
    Write-Host "Embedding Server manifest (uiAccess=$UiAccess) into $binary"
    & $ManifestTool -manifest $manifest "-outputresource:$binary;1"
    if ($LASTEXITCODE -ne 0) {
        throw "Server manifest embedding failed ($LASTEXITCODE)"
    }
    & $ManifestTool "-inputresource:$binary;#1" "-out:$probe"
    if ($LASTEXITCODE -ne 0) {
        throw "Server manifest verification failed ($LASTEXITCODE)"
    }
    $embedded = Get-Content -LiteralPath $probe -Raw
    if ($embedded -notmatch "uiAccess\s*=\s*[`"']$UiAccess[`"']") {
        throw "Embedded Server manifest does not match uiAccess=$UiAccess."
    }
    Write-Host "Verified embedded Server manifest: uiAccess=$UiAccess."
}
finally {
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
    if ($unsignedManifest) { Remove-Item -LiteralPath $unsignedManifest -Force -ErrorAction SilentlyContinue }
}
