param(
    [ValidateRange(30, 240)][int]$ObservationSeconds = 90,
    [ValidateRange(16, 256)][int]$MaxLogMB = 128,
    [ValidateSet('any', 'd3d12', 'vulkan')][string]$GpuBackend = 'any',
    [switch]$HighFrequencyKernelCalls
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$compare = Join-Path $root 'build/xenia-compare'
$app = Join-Path $compare 'app'
$executable = Join-Path $app 'xenia_canary.exe'
$game = Join-Path $root 'game/extracted/default.xex'
$portable = Join-Path $app 'portable.txt'
foreach ($path in @($executable, $game, $portable)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required file missing: $path" }
}
if (Get-Process xenia_canary -ErrorAction SilentlyContinue) {
    throw 'Xenia Canary is already running; close it before this isolated test.'
}
$log = Join-Path $compare ("xenia-save-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$arguments = @($game, "--log_file=$log", '--log_level=3',
    '--flush_log=true',
    "--gpu=$GpuBackend",
    "--log_high_frequency_kernel_calls=$($HighFrequencyKernelCalls.IsPresent.ToString().ToLowerInvariant())",
    '--log_to_stdout=false')
$process = Start-Process -FilePath $executable -ArgumentList $arguments `
    -WorkingDirectory $app -PassThru
$timer = [Diagnostics.Stopwatch]::StartNew()
$peak = 0L
$windowSeen = $false
$reason = $null
try {
    if (!$process.HasExited) { $process.PriorityClass = 'BelowNormal' }
    while (!$process.WaitForExit(200)) {
        $process.Refresh()
        $peak = [Math]::Max($peak, $process.PeakWorkingSet64)
        if ($process.MainWindowHandle -ne 0) { $windowSeen = $true }
        if ($process.WorkingSet64 -gt 3GB) { $reason = '3 GiB working-set limit'; break }
        if ((Test-Path -LiteralPath $log) -and
            (Get-Item -LiteralPath $log).Length -gt ($MaxLogMB * 1MB)) {
            $reason = "$MaxLogMB MiB log limit"; break
        }
        if ($timer.Elapsed.TotalSeconds -ge $ObservationSeconds) {
            $reason = 'observation complete'; break
        }
    }
    $exitedNaturally = $process.HasExited
    $naturalExitCode = if ($exitedNaturally) { $process.ExitCode } else { $null }
    if (!$exitedNaturally) { Stop-Process -Id $process.Id }
    if ($reason -and $reason -ne 'observation complete') { Write-Output "WATCHDOG: $reason" }
    elseif ($exitedNaturally) { Write-Output "Xenia exited before deadline: $naturalExitCode" }
    else { Write-Output 'Observation deadline reached; launcher stopped Xenia' }
    Write-Output "Observed duration: $([Math]::Round($timer.Elapsed.TotalSeconds, 1)) seconds"
    Write-Output "Window observed: $windowSeen"
    Write-Output "Observed peak working set: $peak bytes"
    Write-Output "Xenia debug log: $log"
    Write-Output "Requested GPU backend: $GpuBackend"
    Write-Output "Isolated Xenia storage: $(Join-Path $app 'content')"
} finally {
    if (!$process.HasExited) { Stop-Process -Id $process.Id }
}
if ($reason -and $reason -ne 'observation complete') { exit 124 }
if (!$reason -and $process.ExitCode -ne 0) { exit $process.ExitCode }
