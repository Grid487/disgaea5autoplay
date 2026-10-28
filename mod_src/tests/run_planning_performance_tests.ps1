$ErrorActionPreference = 'Stop'
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$testDir = $PSScriptRoot
$binaryDir = Join-Path $testDir 'build'
New-Item -ItemType Directory -Path $binaryDir -Force | Out-Null
$source = Join-Path $testDir 'planning_performance_tests.cpp'
$exe = Join-Path $binaryDir 'planning_performance_tests.exe'
$obj = Join-Path $binaryDir 'planning_performance_tests.obj'
$command = 'call "' + $vcvars + '" && cl.exe /nologo /std:c++17 /O2 /EHsc /MT /Fo:"' + $obj + '" "' + $source + '" user32.lib /Fe:"' + $exe + '"'
& cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) { throw 'Planning performance regression build failed.' }
$output = & $exe
$runExit = $LASTEXITCODE
$output | ForEach-Object { Write-Host $_ }
$output | Set-Content -LiteralPath (Join-Path $binaryDir 'planning-performance.log') -Encoding UTF8
if ($runExit -ne 0) { throw 'Planning performance regression failed.' }
$results = @($output | Where-Object { $_ -like 'BENCH *' } | ForEach-Object { $_.Substring(6) | ConvertFrom-Json })
@{ scenarios = $results; measuredUtc = (Get-Date).ToUniversalTime().ToString('o'); engine = 'offline stubs'; frameRate = 60 } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $binaryDir 'planning-performance.json') -Encoding UTF8
foreach ($component in @('movement', 'deployment', 'starvation')) {
    $componentSource = Join-Path $testDir ($component + '_performance_tests.cpp')
    $componentExe = Join-Path $binaryDir ($component + '_performance_tests.exe')
    $componentObj = Join-Path $binaryDir ($component + '_performance_tests.obj')
    $componentCommand = 'call "' + $vcvars + '" && cl.exe /nologo /std:c++17 /O2 /EHsc /MT /Fo:"' + $componentObj + '" "' + $componentSource + '" user32.lib /Fe:"' + $componentExe + '"'
    & cmd.exe /d /c $componentCommand
    if ($LASTEXITCODE -ne 0) { throw ($component + ' performance build failed.') }
    $componentOutput = & $componentExe
    $componentExit = $LASTEXITCODE
    $componentOutput | ForEach-Object { Write-Host $_ }
    $componentOutput | Set-Content -LiteralPath (Join-Path $binaryDir ($component + '-performance.log')) -Encoding UTF8
    if ($componentExit -ne 0) { throw ($component + ' performance regression failed.') }
}
