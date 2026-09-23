$ErrorActionPreference = 'Stop'
$forbiddenPaths = @(
    '(^|/)build/', '(^|/)game/', '(^|/)generated/', '(^|/)research/',
    '(^|/)traces/', '(^|/)captures/',
    '\.(iso|xex|xexp|bin|data|pak|arc|xma|xma2|dxbc|dxil|spv|dmp|etl|log)$',
    'shader_cache', 'switch_tables'
)
$tracked = @(git ls-files)
if ($LASTEXITCODE -ne 0) { throw 'Unable to enumerate tracked files.' }
$violations = [System.Collections.Generic.List[string]]::new()
foreach ($path in $tracked) {
    $normalized = $path.Replace('\', '/')
    foreach ($pattern in $forbiddenPaths) {
        if ($normalized -imatch $pattern) {
            $violations.Add("Forbidden tracked path: $path")
            break
        }
    }
    if ((Test-Path -LiteralPath $path -PathType Leaf) -and
        (Get-Item -LiteralPath $path).Length -gt 2MB) {
        $violations.Add("Tracked file exceeds 2 MiB review limit: $path")
    }
}
if ($violations.Count) {
    $violations | ForEach-Object { Write-Error $_ }
    throw "Repository hygiene check failed with $($violations.Count) violation(s)."
}
Write-Host "Repository hygiene check passed for $($tracked.Count) tracked files."
