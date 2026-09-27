param(
    [ValidateRange(3, 60)][int]$ObservationSeconds = 30
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'build/hybrid'
$executable = Join-Path $output 'rrxx_windowed.exe'
$game = Join-Path $root 'game/extracted'
$state = Join-Path $output 'state'
foreach ($path in @($executable, $game)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Required local path missing: $path" }
}
New-Item -ItemType Directory -Force -Path $state | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runtimeLog = Join-Path $output "windowed-runtime-$stamp.log"
$stdout = Join-Path $output "windowed-$stamp.stdout.log"
$stderr = Join-Path $output "windowed-$stamp.stderr.log"
$arguments = @("`"$game`"", "`"$state`"", "--log_file=`"$runtimeLog`"", '--log_level=info')
$process = Start-Process -FilePath $executable -ArgumentList $arguments `
    -WorkingDirectory $output -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr -PassThru
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
        if ($process.WorkingSet64 -gt 2GB) { $reason = '2 GiB working-set limit'; break }
        $logBytes = 0L
        foreach ($path in @($runtimeLog, $stdout, $stderr)) {
            if (Test-Path -LiteralPath $path) { $logBytes += (Get-Item -LiteralPath $path).Length }
        }
        if ($logBytes -gt 16MB) { $reason = '16 MiB log limit'; break }
        if ($timer.Elapsed.TotalSeconds -ge $ObservationSeconds) {
            $reason = 'observation complete'; break
        }
    }
    $exitedNaturally = $process.HasExited
    $exitCode = if ($exitedNaturally) { $process.ExitCode } else { $null }
    if (!$exitedNaturally) { Stop-Process -Id $process.Id }
    if ($reason -and $reason -ne 'observation complete') { Write-Output "WATCHDOG: $reason" }
    elseif ($exitedNaturally) { Write-Output "Process exited before deadline: $exitCode" }
    else { Write-Output 'Observation deadline reached; launcher stopped the process' }
    Write-Output "Observed peak working set: $peak bytes"
    Write-Output "Window observed: $windowSeen"
    Write-Output "Runtime log: $runtimeLog"
} finally {
    if (!$process.HasExited) { Stop-Process -Id $process.Id }
}
if ($reason -and $reason -ne 'observation complete') { exit 124 }
if (!$reason -and $exitCode -ne 0) { exit $exitCode }
