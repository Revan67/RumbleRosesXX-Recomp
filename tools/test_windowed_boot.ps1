param(
    [ValidateRange(3, 1800)][int]$ObservationSeconds = 120,
    [ValidateRange(1, 8)][int]$MemoryLimitGiB = 8,
    [ValidateRange(1, 64)][int]$LogLimitMiB = 64,
    [switch]$FullGuestTrace,
    [switch]$GpuDebug,
    [switch]$GpuValidation,
    [switch]$SafeRelativeConstants,
    [switch]$ContextTrace,
    [switch]$NoMemoryLimit,
    [switch]$FreshState,
    [switch]$CleanSdkCandidate,
    [switch]$PreflightOnly
)
$ErrorActionPreference = 'Stop'
$memoryReason = "$MemoryLimitGiB GiB working-set limit"
$logReason = "$LogLimitMiB MiB log limit"
if ($GpuValidation -and !$GpuDebug) {
    throw '-GpuValidation requires -GpuDebug.'
}
$root = Split-Path $PSScriptRoot -Parent
if ($CleanSdkCandidate -and $FullGuestTrace) {
    throw 'The clean SDK candidate was built without full guest tracing.'
}
$output = Join-Path $root $(if ($CleanSdkCandidate) { 'build/research/host-clean-sdk-validation' }
    elseif ($FullGuestTrace) { 'build/hybrid-trace' } else { 'build/hybrid' })
$stateRoot = if ($FreshState) {
    Join-Path $output ("state-fresh-{0}" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
} elseif ($CleanSdkCandidate) {
    Join-Path $output 'state'
} else {
    Join-Path $root 'build/hybrid/state'
}
$executable = Join-Path $output 'rrxx_windowed.exe'
$gameRoot = Join-Path $root 'game/extracted'
if ($CleanSdkCandidate -or $PreflightOnly) {
    foreach ($path in @($executable, (Join-Path $output 'rexruntime.dll'),
            (Join-Path $output 'rexgpu-xenos.dll'), (Join-Path $gameRoot 'default.xex'))) {
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file missing: $path" }
    }
    if ($CleanSdkCandidate) {
        $sdkRecord = Get-Content -LiteralPath (Join-Path $root 'build/research/sdk-clean-validation-v2/build-result.json') -Raw | ConvertFrom-Json
        foreach ($name in 'rexruntime.dll', 'rexgpu-xenos.dll') {
            $expected = $sdkRecord.outputs."out/win-amd64/Release/$name"
            if (!$expected -or (Get-FileHash -LiteralPath (Join-Path $output $name)).Hash -ne $expected) {
                throw "Candidate SDK identity mismatch: $name"
            }
        }
    }
    Write-Output "Executable: $executable"
    Write-Output "Executable SHA-256: $((Get-FileHash -LiteralPath $executable).Hash)"
    Write-Output "State root: $stateRoot"
    Write-Output "Limits: ${ObservationSeconds}s, $memoryReason, $logReason"
    if ($PreflightOnly) { Write-Output 'Preflight passed; no process launched.'; return }
}
if ($FreshState) { New-Item -ItemType Directory -Path $stateRoot -ErrorAction Stop | Out-Null }
$runtimeLog = Join-Path $output ("windowed-runtime-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$traceFile = if ($FullGuestTrace) { Join-Path $output ("guest-calls-{0}.bin" -f (Get-Date -Format 'yyyyMMdd-HHmmss')) } else { $null }
$inputTraceFile = Join-Path $output ("input-edges-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$drawJournalFile = if ($GpuDebug) { Join-Path $output ("draw-journal-{0}.bin" -f (Get-Date -Format 'yyyyMMdd-HHmmss')) } else { $null }
$arguments = @('"{0}"' -f $gameRoot) + @('"{0}"' -f $stateRoot) +
    @('"--log_file={0}"' -f $runtimeLog) + @('--log_level=debug')
if ($GpuDebug) { $arguments += '--d3d12_debug' }
if ($GpuValidation) { $arguments += '--d3d12_gpu_validation' }
if ($SafeRelativeConstants) { $arguments += '--dxbc_safe_relative_constants=true'; Write-Output 'EXPERIMENT: out-of-range relative float constants return zero.' }
if ($NoMemoryLimit) { Write-Warning 'Working-set limit disabled for this run. Severe system lag is possible.' }
$stdout = Join-Path $output 'windowed.stdout.log'
$stderr = Join-Path $output 'windowed.stderr.log'
$previousTraceFile = [Environment]::GetEnvironmentVariable('RRXX_GUEST_TRACE_FILE', 'Process')
$previousInputTraceFile = [Environment]::GetEnvironmentVariable('RRXX_INPUT_TRACE_FILE', 'Process')
$previousDrawJournal = [Environment]::GetEnvironmentVariable('RRXX_DRAW_JOURNAL', 'Process')
$previousContextTrace = [Environment]::GetEnvironmentVariable('RRXX_CONTEXT_TRACE', 'Process')
try {
    if ($FullGuestTrace) { [Environment]::SetEnvironmentVariable('RRXX_GUEST_TRACE_FILE', $traceFile, 'Process') }
    [Environment]::SetEnvironmentVariable('RRXX_INPUT_TRACE_FILE', $inputTraceFile, 'Process')
    [Environment]::SetEnvironmentVariable('RRXX_DRAW_JOURNAL', $drawJournalFile, 'Process')
    [Environment]::SetEnvironmentVariable('RRXX_CONTEXT_TRACE', $(if ($ContextTrace) { '1' } else { $null }), 'Process')
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $output `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
} finally {
    [Environment]::SetEnvironmentVariable('RRXX_GUEST_TRACE_FILE', $previousTraceFile, 'Process')
    [Environment]::SetEnvironmentVariable('RRXX_INPUT_TRACE_FILE', $previousInputTraceFile, 'Process')
    [Environment]::SetEnvironmentVariable('RRXX_DRAW_JOURNAL', $previousDrawJournal, 'Process')
    [Environment]::SetEnvironmentVariable('RRXX_CONTEXT_TRACE', $previousContextTrace, 'Process')
}
$timer = [Diagnostics.Stopwatch]::StartNew()
$peak = 0L
$handle = 0L
$reason = $null
try {
    if (!$process.HasExited) { $process.PriorityClass = 'BelowNormal' }
    while (!$process.WaitForExit(200)) {
        $process.Refresh()
        $peak = [Math]::Max($peak, $process.PeakWorkingSet64)
        if ($process.MainWindowHandle -ne 0) { $handle = $process.MainWindowHandle }
        if (!$NoMemoryLimit -and $process.WorkingSet64 -gt ($MemoryLimitGiB * 1GB)) { $reason = $memoryReason; break }
        $runtimeLogBytes = if (Test-Path -LiteralPath $runtimeLog) { (Get-Item -LiteralPath $runtimeLog).Length } else { 0 }
        if ((Get-Item $stdout).Length + (Get-Item $stderr).Length + $runtimeLogBytes -gt ($LogLimitMiB * 1MB)) { $reason = $logReason; break }
        if ($timer.Elapsed.TotalSeconds -ge $ObservationSeconds) { $reason = 'observation complete'; break }
    }
    $exitedNaturally = $process.HasExited
    $naturalExitCode = if ($exitedNaturally) { $process.ExitCode } else { $null }
    if (!$exitedNaturally) { Stop-Process -Id $process.Id }
    if ($reason -in @($memoryReason, $logReason)) { Write-Output "WATCHDOG: $reason" }
    elseif ($exitedNaturally) { Write-Output "Process exited before deadline: $naturalExitCode" }
    else { Write-Output 'Observation deadline reached; launcher stopped the process' }
    Write-Output "Observed duration: $([Math]::Round($timer.Elapsed.TotalSeconds, 1)) seconds"
    Write-Output "Window observed: $($handle -ne 0)"
    Write-Output "Observed peak working set: $peak bytes"
    Write-Output "Runtime log: $runtimeLog"
    Write-Output "Input edge log: $inputTraceFile"
    if ($drawJournalFile) { Write-Output "Draw journal: $drawJournalFile" }
    Write-Output "State root: $stateRoot"
    if ($FullGuestTrace) { Write-Output "Guest function trace: $traceFile" }
} finally {
    if (!$process.HasExited) { Stop-Process -Id $process.Id }
}
if ($reason -in @($memoryReason, $logReason)) { exit 124 }
if (!$reason -and $process.ExitCode -ne 0) { exit $process.ExitCode }
