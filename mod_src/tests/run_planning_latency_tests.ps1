$ErrorActionPreference='Stop'
$vcvars='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$out=Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $out -Force|Out-Null
$source=Join-Path $PSScriptRoot 'planning_latency_tests.cpp'
$exe=Join-Path $out 'planning_latency_tests.exe'
$obj=Join-Path $out 'planning_latency_tests.obj'
$command='call "'+$vcvars+'" && cl.exe /nologo /std:c++17 /O2 /EHsc /MT /Fo:"'+$obj+'" "'+$source+'" user32.lib /Fe:"'+$exe+'"'
& cmd.exe /d /c $command
if($LASTEXITCODE -ne 0){throw 'Planning latency test compilation failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Planning latency tests failed'}
