$ErrorActionPreference = 'Stop'
$suiteNames = @(
    'path_query', 'state_regression', 'combat_planner', 'tactical_path',
    'native_combat', 'native_skills', 'native_geometry', 'native_effects',
    'native_tactics', 'native_action_positions', 'native_deployment', 'smart_runtime', 'planning_performance', 'notification', 'stage_repeat', 'native_scenes', 'native_repeat', 'planning_latency'
)
$results = @()
foreach ($suite in $suiteNames) {
    & (Join-Path $PSScriptRoot ('run_' + $suite + '_tests.ps1'))
    $results += @{ suite = $suite; passed = $true }
}
$reportDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
@{ testedUtc = (Get-Date).ToUniversalTime().ToString('o'); suites = $results } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $reportDir 'latest-results.json') -Encoding UTF8
Write-Host ('All ' + $suiteNames.Count + ' offline suites passed.')
