# The CI regression gate of design §17: compares anvil_bench results with
# bench/baseline-ci.json.
#
# CI runners differ in CPU model and clock, so absolute nanoseconds are not
# comparable between runs. The gate compares each case's ratio to the plain
# call measured in the same run, and only against a baseline taken on the same
# CPU model (the "cpu" field of the result). With several result files (runs on
# the same machine), each case takes its lowest ratio: a slow outlier run does
# not fail the gate, a real regression shows in every run.
#
# Exit 0: no case is more than `tolerance` (20%) slower than its baseline, or
# there is no baseline for this CPU model (a notice prints the entry to add).
# Exit 1: a regression, or a gated case is missing from the results.
param(
    [Parameter(Mandatory)][string[]]$Result,
    [string]$Baseline = (Join-Path $PSScriptRoot 'baseline-ci.json')
)

$ErrorActionPreference = 'Stop'
[System.Threading.Thread]::CurrentThread.CurrentCulture = [cultureinfo]::InvariantCulture
$github = [bool]$env:GITHUB_ACTIONS

function Say([string]$kind, [string]$message) {
    if ($github) { Write-Host "::${kind}::$message" } else { Write-Host "${kind}: $message" }
}

$runs = @($Result | ForEach-Object { Get-Content -Raw -LiteralPath $_ | ConvertFrom-Json })
$cpu = $runs[0].cpu
foreach ($run in $runs) {
    if ($run.cpu -ne $cpu) { throw "results come from different CPUs: '$cpu' and '$($run.cpu)'" }
}
$base = Get-Content -Raw -LiteralPath $Baseline | ConvertFrom-Json
$tolerance = [double]$base.tolerance

# The lowest ratio of each case over the runs.
$ratios = [ordered]@{}
foreach ($case in $runs[0].cases.PSObject.Properties.Name) {
    if ($case -eq 'call') { continue }
    $values = @($runs | ForEach-Object { [double]$_.cases.$case.ratio })
    $ratios[$case] = ($values | Measure-Object -Minimum).Minimum
}

$runner = $base.runners.PSObject.Properties[$cpu]
if ($null -eq $runner) {
    $entry = [ordered]@{ source = "anvil_bench $($runs[0].anvil), $($runs.Count) run(s), $(Get-Date -Format yyyy-MM-dd)"; ratios = [ordered]@{} }
    foreach ($k in $ratios.Keys) { $entry.ratios[$k] = [math]::Round($ratios[$k], 3) }
    Say 'notice' "anvil_bench: no CI baseline for the CPU '$cpu'; the regression gate is skipped. To gate this CPU, add the entry printed below under `"runners`" in bench/baseline-ci.json."
    Write-Host (ConvertTo-Json @{ $cpu = $entry } -Depth 4)
    exit 0
}

$failed = $false
$rows = @()
foreach ($case in $runner.Value.ratios.PSObject.Properties) {
    $was = [double]$case.Value
    if (-not $ratios.Contains($case.Name)) {
        Say 'error' "anvil_bench: the case '$($case.Name)' has a baseline but no result; update bench/baseline-ci.json with the benchmark."
        $failed = $true
        continue
    }
    $now = $ratios[$case.Name]
    $change = $now / $was - 1
    $verdict = 'ok'
    if ($change -gt $tolerance) {
        $verdict = 'REGRESSION'
        $failed = $true
        Say 'error' ("anvil_bench: {0} is {1:P0} slower than its baseline on '{2}': {3:N2}x a plain call, baseline {4:N2}x, limit +{5:P0}." -f $case.Name, $change, $cpu, $now, $was, $tolerance)
    } elseif ($change -lt -$tolerance) {
        $verdict = 'faster'
        Say 'notice' ("anvil_bench: {0} is {1:P0} faster than its baseline on '{2}' ({3:N2}x vs {4:N2}x); if this holds, lower its ratio in bench/baseline-ci.json." -f $case.Name, -$change, $cpu, $now, $was)
    }
    $rows += [pscustomobject]@{ case = $case.Name; baseline = [math]::Round($was, 3); now = [math]::Round($now, 3); change = '{0:+0.0%;-0.0%}' -f $change; verdict = $verdict }
}
foreach ($k in $ratios.Keys) {
    if ($null -eq $runner.Value.ratios.PSObject.Properties[$k]) {
        Say 'notice' "anvil_bench: the case '$k' has no baseline on '$cpu' and is not gated yet."
    }
}

$rows | Format-Table -AutoSize | Out-String | Write-Host
if ($env:GITHUB_STEP_SUMMARY) {
    $md = @("### anvil_bench on $cpu", '', "Ratio to a plain call; lowest of $($runs.Count) run(s); limit +$('{0:P0}' -f $tolerance).", '',
        '| case | baseline | now | change | |', '| --- | ---: | ---: | ---: | --- |')
    $md += $rows | ForEach-Object { "| $($_.case) | $($_.baseline) | $($_.now) | $($_.change) | $($_.verdict) |" }
    $md += '', '| per link (design §17) | ns | budget |', '| --- | ---: | ---: |'
    foreach ($b in $runs[0].budgets.PSObject.Properties) {
        $md += "| $($b.Name) | $('{0:N2}' -f [double]$b.Value.ns) | $($b.Value.budget_ns) |"
    }
    $md += '', 'Nanoseconds on a CI runner are indicative only; the binding numbers come from the lab (bench/run-lab.ps1).'
    $md -join "`n" | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY
}
if ($failed) { exit 1 }
