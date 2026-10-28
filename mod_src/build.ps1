param([switch]$StageOnly, [switch]$NativeNotificationQA, [switch]$ProfilePlanning)

$ErrorActionPreference = "Stop"

$vscvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$srcDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$rootDir = Split-Path -Parent $srcDir
$buildRoot = Join-Path $srcDir "build"
$outDll = Join-Path $rootDir "XINPUT9_1_0.dll"

if (-not (Test-Path -LiteralPath $vscvars -PathType Leaf)) {
    throw "Visual Studio x64 build environment not found: $vscvars"
}

# Every compile gets its own staging directory. A compiler failure can only
# leave artifacts here; the currently installed game DLL is not touched.
$buildId = Get-Date -Format "yyyyMMdd-HHmmss-fff"
$stageDir = Join-Path $buildRoot $buildId
if (Test-Path -LiteralPath $stageDir) {
    $stageDir = Join-Path $buildRoot ($buildId + "-" + [guid]::NewGuid().ToString("N"))
}
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
$stagedDll = Join-Path $stageDir "XINPUT9_1_0.dll"
$stagedLib = Join-Path $stageDir "XINPUT9_1_0.lib"
$stagedObj = Join-Path $stageDir "xinput_proxy.obj"

$qaDefine = if ($NativeNotificationQA) { ' /DD5_NATIVE_NOTIFICATION_QA' } else { '' }
if ($ProfilePlanning) { $qaDefine += ' /DD5_QUERY_PROFILE' }
if ($NativeNotificationQA) { Write-Warning 'DEVELOPMENT TEST BUILD: creates two synthetic-text native OK notices. Reinstall a normal build after testing.' }
$cmd = 'call "' + $vscvars + '" && cd /d "' + $srcDir + '" && cl.exe /nologo /std:c++17 /O2 /EHsc /MT /LD' + $qaDefine + ' /Fo:"' + $stagedObj + '" xinput_proxy.cpp user32.lib /link /DEF:xinput9_1_0.def /OUT:"' + $stagedDll + '" /IMPLIB:"' + $stagedLib + '"'
& cmd.exe /d /c $cmd
$compilerExitCode = $LASTEXITCODE
if ($compilerExitCode -ne 0) {
    throw "Compilation failed with exit code $compilerExitCode. The installed game DLL was left unchanged. Staging output: $stageDir"
}
if (-not (Test-Path -LiteralPath $stagedDll -PathType Leaf)) {
    throw "Compilation reported success but produced no staged DLL. The installed game DLL was left unchanged. Staging output: $stageDir"
}

$sourceHashes = @()
Get-ChildItem -LiteralPath $srcDir -File | Where-Object {
    $_.Extension -in @('.h', '.cpp', '.def') -or $_.Name -eq 'build.ps1'
} | Sort-Object Name | ForEach-Object {
    $sourceHashes += @{ file = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
}
$manifest = @{
    version = if ($NativeNotificationQA) { 'v5.4.2-native-notification-qa' } else { 'v5.4.2-low-latency-planning' }
    nativeNotificationQA = [bool]$NativeNotificationQA
    profilePlanning = [bool]$ProfilePlanning
    build = $buildId
    builtUtc = (Get-Date).ToUniversalTime().ToString('o')
    stagedDllSha256 = (Get-FileHash -LiteralPath $stagedDll -Algorithm SHA256).Hash
    executableSha256 = (Get-FileHash -LiteralPath (Join-Path $rootDir 'disgaea5.exe') -Algorithm SHA256).Hash
    sources = $sourceHashes
}
if (Test-Path -LiteralPath $outDll -PathType Leaf) {
    $manifest.installedDllSha256BeforeBuild = (Get-FileHash -LiteralPath $outDll -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stageDir 'build-manifest.json') -Encoding UTF8

if ($StageOnly) {
    Write-Host "Successfully built staged DLL: $stagedDll"
    return
}

# Never try to replace a DLL while the game may have it loaded. Keep the
# successful staged build so it can be installed after the game is closed.
$gameProcess = Get-Process -Name "disgaea5" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($gameProcess) {
    throw "Build succeeded, but Disgaea 5 is still running (PID $($gameProcess.Id)). Close the game and rerun this script to install the staged DLL: $stagedDll"
}

$backupDll = $null
if (Test-Path -LiteralPath $outDll -PathType Leaf) {
    $backupRoot = Join-Path $srcDir "backups\builds"
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $backupId = Get-Date -Format "yyyyMMdd-HHmmss-fff"
    $backupDir = Join-Path $backupRoot $backupId
    if (Test-Path -LiteralPath $backupDir) {
        $backupDir = Join-Path $backupRoot ($backupId + "-" + [guid]::NewGuid().ToString("N"))
    }
    New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    $backupDll = Join-Path $backupDir "XINPUT9_1_0.dll"
    $installedHash = (Get-FileHash -LiteralPath $outDll -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $outDll -Destination $backupDll
    $backupHash = (Get-FileHash -LiteralPath $backupDll -Algorithm SHA256).Hash
    if ($backupHash -ne $installedHash) {
        throw "Could not verify the installed DLL backup at $backupDll. The game DLL was left unchanged."
    }
}

try {
    if (Test-Path -LiteralPath $outDll -PathType Leaf) {
        # ReplaceFile is a single replacement operation. If Windows refuses it
        # because the DLL is locked, the original file is not deleted first.
        # PowerShell converts a null string argument to an empty path here.
        # Reuse the verified backup path instead of passing a null argument.
        if (-not $backupDll) {
            throw "The installed DLL appeared after the backup check. Rerun the build to back it up before replacement."
        }
        [System.IO.File]::Replace($stagedDll, $outDll, $backupDll)
    } else {
        [System.IO.File]::Move($stagedDll, $outDll)
    }
} catch {
    $backupMessage = if ($backupDll) { " Verified backup: $backupDll." } else { "" }
    throw "Build succeeded, but installing the staged DLL failed: $($_.Exception.Message).$backupMessage Staged build: $stagedDll"
}

$installedHashAfterBuild = (Get-FileHash -LiteralPath $outDll -Algorithm SHA256).Hash
if ($installedHashAfterBuild -ne $manifest.stagedDllSha256) {
    throw "Installation hash verification failed for $outDll. Verified previous DLL backup: $backupDll"
}
$manifest.installedUtc = (Get-Date).ToUniversalTime().ToString('o')
$manifest.installedDllSha256 = $installedHashAfterBuild
$manifest.previousDllBackup = $backupDll
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stageDir 'build-manifest.json') -Encoding UTF8

Write-Host "Successfully built and installed $outDll"
Write-Host "Verified installed SHA-256: $installedHashAfterBuild"
if ($backupDll) {
    Write-Host "Previous DLL backup: $backupDll"
}
