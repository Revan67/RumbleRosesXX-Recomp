param([ValidateSet('registration', 'image', 'boot', 'thread-clock', 'offscreen', 'offscreen-clock', 'offscreen-quiet')][string]$Mode = 'registration',
    [ValidateRange(1, 10)][int]$ObservationSeconds = 5)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'build/hybrid'
$arguments = @((Join-Path $root 'game/extracted'), (Join-Path $output 'state'))
if ($Mode -eq 'image') { $arguments += '--load-image' }
if ($Mode -eq 'boot') { $arguments += '--boot-probe' }
if ($Mode -eq 'thread-clock') { $arguments += '--boot-thread-clock' }
if ($Mode -eq 'offscreen') { $arguments += @('--boot-offscreen', "$ObservationSeconds") }
if ($Mode -eq 'offscreen-clock') { $arguments += @('--boot-offscreen-clock', "$ObservationSeconds") }
if ($Mode -eq 'offscreen-quiet') { $arguments += @('--boot-offscreen-quiet', "$ObservationSeconds") }
$stdout = Join-Path $output "$Mode.stdout.log"
$stderr = Join-Path $output "$Mode.stderr.log"
$process = Start-Process -FilePath (Join-Path $output 'rrxx_hybrid.exe') -ArgumentList $arguments -WorkingDirectory $output -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
$timer = [Diagnostics.Stopwatch]::StartNew()
$peak = 0L
$reason = $null
try {
    if (!$process.HasExited) { $process.PriorityClass = 'BelowNormal' }
    while (!$process.WaitForExit(200)) {
        $process.Refresh()
        $peak = [Math]::Max($peak, $process.PeakWorkingSet64)
        if ($timer.Elapsed.TotalSeconds -ge 15) { $reason = '15-second deadline'; break }
        if ($process.WorkingSet64 -gt 2GB) { $reason = '2 GiB working-set limit'; break }
        if ((Get-Item $stdout).Length + (Get-Item $stderr).Length -gt 16MB) { $reason = '16 MiB log limit'; break }
    }
    if ($reason) { Stop-Process -Id $process.Id; Write-Output "WATCHDOG: $reason" }
    else { $process.Refresh(); Write-Output "Exit code: $($process.ExitCode)" }
    Write-Output "Observed peak working set: $peak bytes"
} finally {
    if (!$process.HasExited) { Stop-Process -Id $process.Id }
}
Get-Content $stdout -Tail 30
Get-Content $stderr -Tail 30
if ($reason) { exit 124 }
exit $process.ExitCode
