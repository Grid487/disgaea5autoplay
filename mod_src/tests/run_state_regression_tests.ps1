$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$testDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $testDir "state_regression_tests.cpp"
$binaryDir = Join-Path $testDir "build"
$exe = Join-Path $binaryDir "state_regression_tests.exe"
$obj = Join-Path $binaryDir "state_regression_tests.obj"

New-Item -ItemType Directory -Path $binaryDir -Force | Out-Null
$command = "`"$vcvars`" && cl.exe /nologo /std:c++17 /EHsc /W4 /MT /Fo:`"$obj`" `"$source`" user32.lib /Fe:`"$exe`""
cmd.exe /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Offline state regression build failed with exit code $LASTEXITCODE"
}

& $exe
if ($LASTEXITCODE -ne 0) {
    throw "Offline state regression checks failed with exit code $LASTEXITCODE"
}
