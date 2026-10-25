[CmdletBinding()]
param(
    # PROTOTYPE harness for anvil#134; artifacts go to work/bench-quit/runs/<RunName>.
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$RunName,
    # sdl = the bench-quit plugin pushes SDL_QUIT after ready; wmclose = this script posts WM_CLOSE to
    # the game window after ready; kill = verified TerminateProcess after ready (today's lab way);
    # none = only watch for -WatchSeconds after ready, then kill.
    [Parameter(Mandatory)][ValidateSet('sdl', 'wmclose', 'kill', 'none')][string]$Quit,
    [int]$DelayMs = 3000,
    [switch]$Overlay,
    [switch]$Bellows,
    # Start without SteamAppId/SteamGameId in the child's environment.
    [switch]$NoSteamAppId,
    # A settings.txt to put in place for this start instead of the baseline (e.g. the one a kill left);
    # it must differ from the baseline only in refreshRate/graceful_exit.
    [string]$SettingsFrom = '',
    [int]$WatchSeconds = 20,
    [int]$TimeoutSeconds = 150
)
$ErrorActionPreference = 'Stop'
$lab = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location -LiteralPath $lab
Import-Module (Join-Path $lab 'scripts\lib\LabProcess.psm1') -Force
Import-Module (Join-Path $lab 'scripts\lib\StartupBaseline.psm1') -Force
Add-Type -AssemblyName System.Drawing
if (-not ([System.Management.Automation.PSTypeName]'BenchQuitWin').Type) {
    Add-Type -Namespace BQ -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr lParam);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, System.Text.StringBuilder s, int n);
[DllImport("kernel32.dll")] public static extern bool GetProcessTimes(IntPtr p, out long c, out long e, out long k, out long u);
[DllImport("kernel32.dll")] public static extern bool GetExitCodeProcess(IntPtr p, out uint code);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
'@
}

$knownSha = '9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A'
$gameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Europa Universalis IV'
$copy = Join-Path $lab 'work\anvil-test-v1'
$exe = Join-Path $copy 'eu4.exe'
$userDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Paradox Interactive\Europa Universalis IV'
$settings = Join-Path $userDir 'settings.txt'
$systemLog = Join-Path $userDir 'logs\system.log'
$runDir = Join-Path $lab "work\bench-quit\runs\$RunName"
if (Test-Path $runDir) { throw "Run exists: $runDir" }
$anvilRelease = Join-Path $lab 'work\bellows-ws-test\anvil-build'   # Anvil 1.3.0 release: version.dll + plugin\anvil-game
$plugins = @((Join-Path $anvilRelease 'plugin\anvil-game'), (Join-Path $lab 'work\bench-quit\build\plugin\bench-quit'))
if ($Overlay) { $plugins += Join-Path $lab 'work\bellows-ws-test\anvil-overlay' }
if ($Bellows) { $plugins += 'C:\Program Files (x86)\Steam\steamapps\workshop\content\236850\3823747170\anvil\bellows' }

# Preflight (AGENTS.md, startup): Process Lasso baseline, hashes of the original, the copy and the DLLs.
Assert-NoGameRunning -ExePath $exe | Out-Null
Assert-LabPreflight -ExePath (Join-Path $gameDir 'eu4.exe') -ExpectedHash $knownSha | Out-Null
Assert-LabPreflight -ExePath $exe -ExpectedHash $knownSha | Out-Null
foreach ($j in Get-ChildItem $copy -Directory | Where-Object Name -ne 'anvil') {
    if (-not ($j.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Not a junction: $($j.FullName)" }
}
New-Item -ItemType Directory $runDir -Force | Out-Null
$hashes = [ordered]@{ originalExe = $knownSha; copyExe = $knownSha; versionDll = (Get-FileHash (Join-Path $anvilRelease 'version.dll')).Hash }
foreach ($p in $plugins) { foreach ($d in Get-ChildItem $p -Filter *.dll) { $hashes["$(Split-Path $p -Leaf)/$($d.Name)"] = (Get-FileHash $d.FullName).Hash } }
$steam = @(Get-Process steam -ErrorAction SilentlyContinue).Count -gt 0

$baseline = Enter-StartupBaseline   # throws before anything changes
if ($SettingsFrom) {
    $diff = @(Compare-StartupSettings -Text ([IO.File]::ReadAllText((Resolve-Path $SettingsFrom))) -BaselineText ([IO.File]::ReadAllText($baseline.BaselineSettings)))
    if ($diff.Count) { throw "SettingsFrom differs from the baseline outside the allowed keys: $($diff -join '; ')" }
    Copy-Item $SettingsFrom $settings
}
Copy-Item $settings (Join-Path $runDir 'settings-before.txt')

$proxy = Join-Path $copy 'version.dll'; $anvilDir = Join-Path $copy 'anvil'
foreach ($n in $proxy, $anvilDir) { if (Test-Path $n) { Remove-Item -LiteralPath $n -Recurse } }
Copy-Item (Join-Path $anvilRelease 'version.dll') $proxy
New-Item -ItemType Directory (Join-Path $anvilDir 'plugins') -Force | Out-Null
foreach ($p in $plugins) { Copy-Item $p (Join-Path $anvilDir 'plugins') -Recurse }

$events = New-Object 'System.Collections.Generic.List[object]'
function Note([string]$What, $Extra = $null) {
    $e = [ordered]@{ t = [math]::Round(((Get-Date) - $script:started).TotalSeconds, 3); what = $What }
    if ($Extra) { $e.extra = $Extra }
    $events.Add($e); Write-Output ("[{0,7:N2}] {1} {2}" -f $e.t, $What, $(if ($Extra) { $Extra | ConvertTo-Json -Compress } else { '' }))
}
function Get-GameWindows([int]$GamePid) {
    $script:wins = New-Object 'System.Collections.Generic.List[object]'
    $cb = [BQ.Win+EnumProc]{
        param($h, $l)
        $owner = 0; [void][BQ.Win]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -eq $GamePid) {
            $r = New-Object BQ.Win+RECT; [void][BQ.Win]::GetClientRect($h, [ref]$r)
            $t = New-Object Text.StringBuilder 256; [void][BQ.Win]::GetWindowTextW($h, $t, 256)
            $c = New-Object Text.StringBuilder 256; [void][BQ.Win]::GetClassNameW($h, $c, 256)
            $script:wins.Add([pscustomobject]@{ hwnd = $h; visible = [BQ.Win]::IsWindowVisible($h); w = $r.Right; h = $r.Bottom; title = "$t"; class = "$c" })
        }
        return $true
    }
    [void][BQ.Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:wins.ToArray()
}
function Save-Shot([int]$GamePid, [string]$Name) {
    $w = Get-GameWindows $GamePid | Where-Object visible | Sort-Object { $_.w * $_.h } -Descending | Select-Object -First 1
    if (-not $w) { return }
    $r = New-Object BQ.Win+RECT; [void][BQ.Win]::GetWindowRect($w.hwnd, [ref]$r)
    $bmp = New-Object Drawing.Bitmap ([math]::Max(1, $r.Right - $r.Left)), ([math]::Max(1, $r.Bottom - $r.Top))
    $g = [Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][BQ.Win]::PrintWindow($w.hwnd, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $small = New-Object Drawing.Bitmap $bmp, ([int]($bmp.Width / 2)), ([int]($bmp.Height / 2))
    $small.Save((Join-Path $runDir "$Name.png"), [Drawing.Imaging.ImageFormat]::Png); $small.Dispose(); $bmp.Dispose()
}

$game = $null; $status = 'started'; $readyAt = $null; $anvilReadyAt = $null; $quitAt = $null; $exitAt = $null; $exitSeen = $null
$exitCode = $null; $closeOutcome = $null; $children = @(); $restore = $null; $windowsAtReady = @(); $laterGames = @()
$env0 = [ordered]@{ BENCH_QUIT_MODE = $(if ($Quit -eq 'sdl') { 'sdl' } else { 'off' }); BENCH_QUIT_DELAY_MS = "$DelayMs" }
if ($NoSteamAppId) { $env0.SteamAppId = ''; $env0.SteamGameId = '' }
$script:started = Get-Date
try {
    $game = Start-LabGame -ExePath $exe -WorkingDirectory $gameDir -AffinityMask 0xFE -Environment $env0
    Note 'created' @{ pid = $game.Pid; env = $game.EnvironmentNames; steamRunning = $steam }
    $anvilLog = Join-Path $anvilDir 'anvil.log'
    while (((Get-Date) - $started).TotalSeconds -lt $TimeoutSeconds) {
        Start-Sleep -Milliseconds 250
        if (-not (Test-VerifiedSessionAlive $game)) { $status = 'exited-before-ready'; break }
        if (-not $readyAt -and (Test-Path $systemLog) -and (Get-Item $systemLog).LastWriteTime -gt $started -and
            (Select-String -LiteralPath $systemLog -Pattern 'Running application' -Quiet)) { $readyAt = Get-Date; Note 'Running application' }
        if (-not $anvilReadyAt -and (Test-Path $anvilLog) -and (Select-String -LiteralPath $anvilLog -Pattern 'game ready at' -Quiet)) { $anvilReadyAt = Get-Date; Note 'anvil game ready' }
        if ($readyAt -and $anvilReadyAt) { $status = 'ready'; break }
    }
    if ($status -eq 'started') { $status = 'timeout'; Save-Shot $game.Pid 'timeout' }
    if ($status -eq 'ready') {
        Start-Sleep -Milliseconds 500
        $windowsAtReady = @(Get-GameWindows $game.Pid | ForEach-Object { [ordered]@{ visible = $_.visible; w = $_.w; h = $_.h; title = $_.title; class = $_.class } })
        Note 'windows' @{ count = $windowsAtReady.Count; visible = @($windowsAtReady | Where-Object visible).Count }
        Save-Shot $game.Pid 'ready'
        $steamModules = @((Get-Process -Id $game.Pid).Modules | Where-Object ModuleName -match 'steam|overlay' | ForEach-Object ModuleName | Sort-Object)
        Note 'steam modules' @{ modules = $steamModules }
        switch ($Quit) {
            'wmclose' {
                Start-Sleep -Milliseconds ([math]::Max(0, $DelayMs - 500))
                Assert-VerifiedSession $game | Out-Null
                $w = Get-GameWindows $game.Pid | Where-Object visible | Sort-Object { $_.w * $_.h } -Descending | Select-Object -First 1
                $quitAt = Get-Date
                $ok = [BQ.Win]::PostMessageW($w.hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
                Note 'WM_CLOSE posted' @{ ok = $ok; class = $w.class; title = $w.title }
            }
            'kill' {
                Start-Sleep -Milliseconds ([math]::Max(0, $DelayMs - 500))
                $quitAt = Get-Date
                $closeOutcome = Stop-VerifiedSession $game
                Note 'killed' @{ outcome = $closeOutcome }
            }
            'sdl' { Note 'waiting for the plugin to quit' }
            'none' { Note "watching $WatchSeconds s" }
        }
        $limit = if ($Quit -eq 'none') { $WatchSeconds } else { 60 + $DelayMs / 1000 }
        $t0 = Get-Date
        while (((Get-Date) - $t0).TotalSeconds -lt $limit -and (Test-VerifiedSessionAlive $game)) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-VerifiedSessionAlive $game)) { $exitSeen = Get-Date }
        if (Test-VerifiedSessionAlive $game) {
            Save-Shot $game.Pid 'not-exited'
            $status = if ($Quit -eq 'none') { 'watched' } else { 'did-not-exit' }
            $closeOutcome = Stop-VerifiedSession $game
            Note 'killed after the wait' @{ outcome = $closeOutcome }
        } else {
            if ($Quit -ne 'kill') { $status = 'exited' } else { $status = 'killed' }
        }
    }
    # Exit time and code from the process itself.
    [long]$ct = 0; [long]$xt = 0; [long]$kt = 0; [long]$ut = 0
    $timesOk = [LabProcessNative]::GetProcessTimes($game.Handle, [ref]$ct, [ref]$xt, [ref]$kt, [ref]$ut)
    $exitAt = if ($timesOk -and $xt -gt 0) { [DateTime]::FromFileTimeUtc($xt).ToLocalTime() } else { $exitSeen }
    $code = [uint32]0; if ([LabProcessNative]::GetExitCodeProcess($game.Handle, [ref]$code)) { $exitCode = $code }
    Note 'process exit' @{ exitCode = $exitCode; exitFileTimeUtc = $xt; timesOk = $timesOk; handle = [int64]$game.Handle }
    # A self-restart: a child eu4.exe of this session, or any new game process, for 15 s.
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 15) {
        $kids = @(Get-VerifiedSessionChild -Session $game -ExePath $exe)
        $any = @(Get-RunningLabGame -ExePath $exe)
        if ($kids.Count -or $any.Count) { $laterGames = @($any | ForEach-Object Id); break }
        Start-Sleep -Milliseconds 500
    }
    $children = @(Stop-RestartedGame -Session $game -ExePath $exe)
    Note 'restart check' @{ children = @($children).Count; otherGames = $laterGames }
}
finally {
    if ($game -and (Test-VerifiedSessionAlive $game)) { $closeOutcome = Stop-VerifiedSession $game; Note 'finally kill' @{ outcome = $closeOutcome } }
    if (-not $game -or -not (Test-VerifiedSessionAlive $game)) {
        Copy-Item $settings (Join-Path $runDir 'settings-after.txt')
        foreach ($n in 'anvil.log', 'anvil.prev.log') { if (Test-Path (Join-Path $anvilDir $n)) { Copy-Item (Join-Path $anvilDir $n) $runDir } }
        if (Test-Path (Join-Path $anvilDir 'data')) { Copy-Item (Join-Path $anvilDir 'data') (Join-Path $runDir 'anvil-data') -Recurse }
        foreach ($n in 'system.log', 'setup.log', 'error.log', 'game.log') {
            $f = Join-Path $userDir "logs\$n"; if ((Test-Path $f) -and (Get-Item $f).LastWriteTime -gt $started) { Copy-Item $f $runDir }
        }
        $restore = Exit-StartupBaseline $baseline -RunDir $runDir
        if (-not $restore.Restored) { Write-Warning "settings.txt not restored: $($restore.Reason) ($($restore.UnexpectedDiff -join '; '))" }
        foreach ($n in $proxy, $anvilDir) { if (Test-Path $n) { Remove-Item -LiteralPath $n -Recurse } }
    } else { Write-Warning "PID $($game.Pid) still alive ($closeOutcome); nothing restored" }
    if ($game) { Close-VerifiedSession $game }
}
if ($Quit -eq 'sdl' -and (Test-Path (Join-Path $runDir 'anvil.log'))) {
    $m = Select-String (Join-Path $runDir 'anvil.log') -Pattern 'SDL_QUIT pushed .* utc_filetime ([0-9]+)' | Select-Object -First 1
    if ($m) { $quitAt = [DateTime]::FromFileTimeUtc([int64]$m.Matches[0].Groups[1].Value).ToLocalTime() }
}
$rawDiff = @(Compare-Object (Get-Content (Join-Path $runDir 'settings-before.txt')) (Get-Content (Join-Path $runDir 'settings-after.txt')) |
    ForEach-Object { "$($_.SideIndicator) $($_.InputObject.Trim())" })
$result = [ordered]@{
    runName = $RunName; quit = $Quit; delayMs = $DelayMs; overlay = [bool]$Overlay; bellows = [bool]$Bellows; noSteamAppId = [bool]$NoSteamAppId
    steamRunning = $steam; settingsFrom = $SettingsFrom; status = $status; hashes = $hashes; affinityMask = '0xFE'
    pid = $game.Pid; createdUtc = $game.CreationUtc.ToString('o')
    runningApplicationS = if ($readyAt) { [math]::Round(($readyAt - $started).TotalSeconds, 2) } else { $null }
    anvilReadyS = if ($anvilReadyAt) { [math]::Round(($anvilReadyAt - $started).TotalSeconds, 2) } else { $null }
    quitToExitS = if ($quitAt -and $exitAt) { [math]::Round(($exitAt - $quitAt).TotalSeconds, 2) } else { $null }
    readyToExitS = if ($anvilReadyAt -and $exitAt) { [math]::Round(($exitAt - $anvilReadyAt).TotalSeconds, 2) } else { $null }
    exitCode = $exitCode; closeOutcome = $closeOutcome; windowsAtReady = $windowsAtReady
    restartedChildren = @($children | ForEach-Object { [ordered]@{ pid = $_.pid; outcome = $_.outcome } }); otherGamesAfterExit = $laterGames
    settingsDiffVsBefore = $rawDiff; settingsRestored = [bool]($restore -and $restore.Restored); unexpectedDiff = @($restore.UnexpectedDiff)
    events = $events.ToArray()
}
$result | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $runDir 'result.json') -Encoding UTF8
Write-Output ("{0}: {1}, ready {2} s, ready->exit {3} s, exit code {4}, settings diff [{5}], restored={6}, restarts={7}" -f
    $RunName, $status, $result.anvilReadyS, $result.readyToExitS, $exitCode, ($rawDiff -join ' | '), $result.settingsRestored, @($children).Count)
