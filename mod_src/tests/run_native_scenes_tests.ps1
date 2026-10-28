$ErrorActionPreference = 'Stop'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $PSScriptRoot 'native_scenes_tests.cpp'
$binaryDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $binaryDir -Force | Out-Null
$exe = Join-Path $binaryDir 'native_scenes_tests.exe'
$obj = Join-Path $binaryDir 'native_scenes_tests.obj'
$command = '"' + $vcvars + '" && cl.exe /nologo /std:c++17 /EHsc /W4 /MT /Fo:"' + $obj + '" "' + $source + '" user32.lib /Fe:"' + $exe + '"'
& cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) { throw 'Native scene regression build failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Native scene regressions failed' }
