[CmdletBinding()]
param([string]$IsccPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $IsccPath) {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) { $IsccPath = $command.Source }
}
if (-not $IsccPath) {
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, (Join-Path $env:LOCALAPPDATA 'Programs'))) {
        if (-not $base) { continue }
        $candidate = Join-Path $base 'Inno Setup 6/ISCC.exe'
        if (Test-Path -LiteralPath $candidate) { $IsccPath = $candidate; break }
    }
}
if (-not $IsccPath) { throw '找不到 ISCC.exe；可用 -IsccPath 指定编译器' }

$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $temporaryRoot ('msime-webview2-probe-' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
try {
    foreach ($location in @('machine32/152.0.1.0', 'machine64/153.0.1.0', 'user/154.0.1.0')) {
        $directory = Join-Path $work $location
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $directory 'msedgewebview2.exe'), 'test fixture')
    }
    $source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../msime_setup.iss'))
    $functions = [regex]::Match($source, '(?s)function IsWebView2Version\(.*?(?=\r?\n// 用注册表判定)').Value
    if (-not $functions) { throw '找不到生产 WebView2 检测函数' }
    # Run the production algorithm and real file checks. Substitute only registry reads and
    # install-root expansion so the test never changes EdgeUpdate registration or installed files.
    $functions = $functions.Replace('RegQueryStringValue(', 'ProbeRegistryVersion(')
    $functions = $functions.Replace('ExpandConstant(', 'ProbeInstallRoot(')
    $resultPath = Join-Path $work 'result.txt'
    $probePath = Join-Path $work 'probe.iss'
    @"
[Setup]
AppName=WebView2 detection probe
AppVersion=1.0
CreateAppDir=no
PrivilegesRequired=lowest
Uninstallable=no
OutputDir=$work
OutputBaseFilename=webview2-probe
[Code]
const
  WebView2ClientKey = 'test-only';
var
  Machine32Version, Machine64Version, UserVersion: String;

function ProbeRegistryVersion(RootKey: Integer; const Key, Name: String; var Value: String): Boolean;
begin
  Value := '';
  if RootKey = HKLM32 then Value := Machine32Version;
  if RootKey = HKLM64 then Value := Machine64Version;
  if RootKey = HKCU then Value := UserVersion;
  Result := Value <> '';
end;

function ProbeInstallRoot(const Name: String): String;
begin
  if Name = '{pf32}\Microsoft\EdgeWebView\Application' then Result := '$work\machine32'
  else if Name = '{pf64}\Microsoft\EdgeWebView\Application' then Result := '$work\machine64'
  else if Name = '{localappdata}\Microsoft\EdgeWebView\Application' then Result := '$work\user'
  else RaiseException('Unexpected install root: ' + Name);
end;

$functions

procedure ExpectVersion(const Expected, Scenario: String);
var
  Actual: String;
begin
  Actual := ReadWebView2Version;
  if Actual <> Expected then
    RaiseException(Scenario + ': expected [' + Expected + '], got [' + Actual + ']');
end;

function InitializeSetup: Boolean;
begin
  try
    Machine32Version := '';
    Machine64Version := '';
    UserVersion := '';
    ExpectVersion('', 'not installed');
    Machine32Version := '152.0.1.0';
    ExpectVersion('152.0.1.0', 'valid machine installation');
    Machine32Version := '151.0.1.0';
    ExpectVersion('', 'stale registration with missing files');
    Machine32Version := '0.0.0.0';
    Machine64Version := '153.0.1.0';
    ExpectVersion('153.0.1.0', 'zero version falls through to native installation');
    Machine32Version := '../152.0.1.0';
    ExpectVersion('153.0.1.0', 'invalid primary version does not hide valid fallback');
    Machine32Version := '151.0.1.0';
    Machine64Version := '151.0.1.0';
    UserVersion := '154.0.1.0';
    ExpectVersion('154.0.1.0', 'stale machine keys do not hide per-user installation');
    Machine32Version := ' 152.0.1.0 ';
    ExpectVersion('152.0.1.0', 'whitespace trimmed');
    if IsWebView2Version('152..1.0') or IsWebView2Version('152.1.0.') or
       IsWebView2Version('152.1.0') or IsWebView2Version('152.1.0.0.1') or
       IsWebView2Version('0.0.0.0') or IsWebView2Version('152/1.0.0') then
      RaiseException('Malformed versions accepted');
    SaveStringToFile('$resultPath', 'ok', False);
  except
    SaveStringToFile('$resultPath', GetExceptionMessage, False);
  end;
  Result := False;
end;
"@ | Set-Content -LiteralPath $probePath -Encoding utf8BOM
    & $IsccPath /Q $probePath
    if ($LASTEXITCODE -ne 0) {
        $compileExit = $LASTEXITCODE
        $line = 0
        foreach ($text in [IO.File]::ReadAllLines($probePath)) {
            $line++
            Write-Host ("{0}: {1}" -f $line, $text)
        }
        throw "ISCC compile failed: $compileExit"
    }
    $process = Start-Process -FilePath (Join-Path $work 'webview2-probe.exe') -WindowStyle Hidden -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES' -PassThru -Wait
    if (-not (Test-Path -LiteralPath $resultPath)) { throw "Probe did not run: $($process.ExitCode)" }
    $result = [IO.File]::ReadAllText($resultPath)
    if ($result -ne 'ok') { throw $result }
    Write-Host 'WebView2 runtime probe passed: stale keys, missing files, alternate scope, malformed versions'
} finally {
    $resolvedWork = [IO.Path]::GetFullPath($work)
    if (-not $resolvedWork.StartsWith((Join-Path $temporaryRoot 'msime-webview2-probe-'), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected probe path: $resolvedWork"
    }
    Remove-Item -LiteralPath $resolvedWork -Recurse -Force
}
