# Runs anvil_bench the way design §17 binds the numbers: on the lab machine
# (i9-13900KF), the benchmark thread pinned to logical CPU 4 at high priority,
# with at least 30 repetitions (median reported), and EU4 closed.
#
#   pwsh bench/run-lab.ps1 [-Bench build/Release/anvil_bench.exe] [-Repetitions 31] [-Out bench-lab.json]
#
# Writes the anvil_bench JSON with a "lab" section (commit, SHA-256 of the
# benchmark, start time, Windows build) and prints the per-link costs against
# the budgets. Exit 1 when a budget is exceeded, 2 when the conditions are not met.
param(
    [string]$Bench = (Join-Path $PSScriptRoot '..\build\Release\anvil_bench.exe'),
    [int]$Repetitions = 31,
    [int]$Cpu = 4,
    [double]$SampleMs = 2,
    [string]$Out = ("bench-lab-{0}.json" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
)

$ErrorActionPreference = 'Stop'
[System.Threading.Thread]::CurrentThread.CurrentCulture = [cultureinfo]::InvariantCulture

if ($Repetitions -lt 30) {
    Write-Error "design §17 needs a median of at least 30 repetitions (got $Repetitions)"
    exit 2
}
if (-not (Test-Path -LiteralPath $Bench)) {
    Write-Error "anvil_bench not found at $Bench; build it first (cmake --build build --config Release --target anvil_bench)"
    exit 2
}
if (Get-Process -Name eu4 -ErrorAction SilentlyContinue) {
    Write-Error 'eu4.exe is running; close the game before a lab measurement'
    exit 2
}
if ($Cpu -ge [Environment]::ProcessorCount) {
    Write-Error "CPU $Cpu does not exist on this machine ($([Environment]::ProcessorCount) logical CPUs)"
    exit 2
}

$started = Get-Date
& $Bench --repetitions $Repetitions --cpu $Cpu --sample-ms $SampleMs --out $Out
if ($LASTEXITCODE -ne 0) {
    Write-Error "anvil_bench failed with exit code $LASTEXITCODE"
    exit 2
}

$result = Get-Content -Raw -LiteralPath $Out | ConvertFrom-Json
$commit = $null
try { $commit = (git -C $PSScriptRoot rev-parse HEAD 2>$null) } catch {}
$result | Add-Member -NotePropertyName lab -NotePropertyValue ([ordered]@{
    commit       = $commit
    bench_sha256 = (Get-FileHash -LiteralPath $Bench -Algorithm SHA256).Hash.ToLower()
    started      = $started.ToString('o')
    seconds      = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    windows      = [Environment]::OSVersion.Version.ToString()
})
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Out -Encoding utf8

if ($result.cpu -notmatch 'i9-13900KF') {
    Write-Warning "not the lab CPU ($($result.cpu)): the numbers are not binding"
}
$over = @($result.budgets.PSObject.Properties | Where-Object { -not $_.Value.within })
Write-Host ''
Write-Host "Result: $Out ($($result.repetitions) repetitions, CPU $Cpu, $($result.cpu))"
foreach ($b in $over) {
    Write-Host ("over budget: {0} {1:N2} ns > {2} ns" -f $b.Name, [double]$b.Value.ns, $b.Value.budget_ns)
}
if ($over.Count -gt 0) { exit 1 }
