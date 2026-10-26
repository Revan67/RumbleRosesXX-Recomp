$ErrorActionPreference = "Stop"

$forbiddenPaths = @(
    '(^|/)build/',
    '(^|/)game/',
    '(^|/)generated/',
    '(^|/)(research|traces|captures)/',
    '(^|/)AGENTS\.md$',
    '^docs/(?!FIRST_PLAYABLE_CHECKPOINT\.md$)',
    '(^|/)Rumble Roses XX',
    'config/rrxx_switch_tables\.toml$',
    '\.(iso|xex|xexp|bin|data|pak|arc|xma|xma2|dxbc|dxil|spv|dmp|etl|log|bundle|sqlite|db)$',
    'shader_cache'
)

$tracked = @(git ls-files)
if ($LASTEXITCODE -ne 0) {
    throw "Unable to enumerate tracked files."
}

$violations = [System.Collections.Generic.List[string]]::new()
foreach ($path in $tracked) {
    $normalized = $path.Replace('\', '/')
    foreach ($pattern in $forbiddenPaths) {
        if ($normalized -imatch $pattern) {
            $violations.Add("Forbidden tracked path: $path")
            break
        }
    }

    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $item = Get-Item -LiteralPath $path
        if ($item.Length -gt 2MB) {
            $violations.Add("Tracked file exceeds 2 MiB review limit: $path ($($item.Length) bytes)")
        }
    }
}

$machineNamePattern = 'YAMA' + 'SHIRO'
$profileNamePattern = 'Jo' + 'el'
$windowsProfilePattern = 'C:' + '\\' + '\\' + 'Users' + '\\' + '\\'
$slashProfilePattern = 'C:' + '/' + 'Users' + '/'
$identityPattern = "$machineNamePattern|$profileNamePattern|$windowsProfilePattern|$slashProfilePattern"
$identityHits = @(git grep -n -I -E $identityPattern -- 2>$null)
if ($LASTEXITCODE -eq 0) {
    foreach ($hit in $identityHits) {
        $violations.Add("Machine identity or absolute user path: $hit")
    }
}

if ($violations.Count -ne 0) {
    $violations | ForEach-Object { Write-Error $_ }
    throw "Repository hygiene check failed with $($violations.Count) violation(s)."
}

Write-Host "Repository hygiene check passed for $($tracked.Count) tracked files."
exit 0
