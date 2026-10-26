param(
    [string]$OutputDirectory = 'build/research/switch-boundaries'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if (!$output.StartsWith((Join-Path $root 'build') + [IO.Path]::DirectorySeparatorChar,
    [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be below project build/.' }
New-Item -ItemType Directory -Force -Path $output | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $output 'generated') | Out-Null
$imagePath = Join-Path $root 'game/rrxx_image.bin'
$switchPath = Join-Path $root 'config/rrxx_switch_tables.toml'
$generator = Join-Path $root 'tools/XenonRecomp/build/XenonRecomp/XenonRecomp.exe'
# Title-specific metadata; table payloads are read from private input, never embedded.
$specs = @(
    @{ Entry=0x820AEC40L; End=0x820AECB0L; Table=0x820AEC80L; Count=4; Register=11 },
    @{ Entry=0x82146560L; End=0x82147034L; Table=0x821469ECL; Count=12; Register=7 },
    @{ Entry=0x8214E878L; End=0x8214E9ACL; Table=0x8214E938L; Count=4; Register=5 },
    @{ Entry=0x824080C0L; End=0x82408700L; Table=0x82408544L; Count=21; Register=19 }
)
$stream = [IO.File]::OpenRead($imagePath)
try {
    $reader = [IO.BinaryReader]::new($stream)
    foreach ($spec in $specs) {
        $stream.Position = $spec.Table - 0x82000000L
        $spec.Labels = @(for ($index=0; $index -lt $spec.Count; ++$index) {
            $bytes = $reader.ReadBytes(4)
            if ($bytes.Length -ne 4) { throw 'Truncated table' }
            [Array]::Reverse($bytes)
            $target = [int64][BitConverter]::ToUInt32($bytes, 0)
            if (($target -band 3) -or $target -lt $spec.Entry -or $target -ge $spec.End) {
                throw ('Target {0:X8} escapes function {1:X8}' -f $target, $spec.Entry)
            }
            $target
        })
        $stream.Position = $spec.Table - 0x82000000L - 4
        if ([BitConverter]::ToString($reader.ReadBytes(4)) -ne '4E-80-04-20') {
            throw 'Table is not immediately preceded by bctr'
        }
    }
} finally { $stream.Dispose() }
$switches = [IO.File]::ReadAllText($switchPath)
foreach ($spec in $specs) {
    $dispatch = $spec.Table - 4
    if ($switches -match ('(?i)base\s*=\s*0x{0:X8}\b' -f $dispatch)) {
        throw 'Dispatch already configured; review before replacing it'
    }
    $labels = ($spec.Labels | ForEach-Object { '0x{0:X8}' -f $_ }) -join ', '
    $switches += "`n[[switch]]`nbase = 0x{0:X8}`nr = {1}`nlabels = [{2}]`n" -f $dispatch,$spec.Register,$labels
}
[IO.File]::WriteAllText((Join-Path $output 'switches.toml'), $switches)
$relativeRoot = [IO.Path]::GetRelativePath($output, $root).Replace('\','/')
$config = "[main]`nfile_path = `"$relativeRoot/game/extracted/default.xex`"`nout_directory_path = `"generated`"`nswitch_table_file_path = `"switches.toml`"`nfunctions = [`n"
foreach ($spec in $specs) {
    $config += '  {{ address = 0x{0:X8}, size = 0x{1:X} }},' -f $spec.Entry,($spec.End-$spec.Entry)
    $config += "`n"
}
$config += "]`n"
# Preserve the existing conservative code-generation settings and ABI helpers.
$settings = [IO.File]::ReadAllText((Join-Path $root 'config/rrxx.toml'))
$settings = $settings.Substring($settings.IndexOf('skip_lr ='))
$config += $settings
$configPath = Join-Path $output 'recompile.toml'
[IO.File]::WriteAllText($configPath, $config)
$stdout = Join-Path $output 'generator.stdout.log'
$stderr = Join-Path $output 'generator.stderr.log'
$header = Join-Path $root 'tools/XenonRecomp/XenonUtils/ppc_context.h'
$process = Start-Process -FilePath $generator -ArgumentList @($configPath,$header) `
    -WorkingDirectory $root -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try {
    if (!$process.HasExited) {
        $process.PriorityClass = 'BelowNormal'
        $process.ProcessorAffinity = [intptr]3
    }
    if (!$process.WaitForExit(60000)) { throw 'Generator exceeded 60-second deadline' }
    if ($process.ExitCode -ne 0) { throw "Generator failed: $($process.ExitCode)" }
} finally { if (!$process.HasExited) { Stop-Process -Id $process.Id } }
$units = @(Get-ChildItem (Join-Path $output 'generated') -Filter 'ppc_recomp.*.cpp')
$records = @()
foreach ($spec in $specs) {
    $name = 'sub_{0:X8}' -f $spec.Entry
    $pattern = '(?ms)^PPC_FUNC_IMPL\(__imp__' + $name + '\) \{.*?^\}'
    $bodies = @(foreach ($unit in $units) {
        [regex]::Matches([IO.File]::ReadAllText($unit.FullName), $pattern) | ForEach-Object Value
    })
    if ($bodies.Count -ne 1) { throw "Expected exactly one generated body for $name" }
    $body = $bodies[0]
    if ($body -match '// ERROR') { throw "Incomplete generated boundary: $name" }
    $dispatchPattern = '(?s)// bctr\s*switch \(ctx\.r' + $spec.Register + '\.u64\) \{(?<cases>.*?)default:'
    $dispatches = @([regex]::Matches($body,$dispatchPattern) | Where-Object {
        $candidate = $_.Groups['cases'].Value
        $matchesTable = [regex]::Matches($candidate,'case \d+:').Count -eq $spec.Count
        for ($caseIndex=0; $caseIndex -lt $spec.Count; ++$caseIndex) {
            $matchesTable = $matchesTable -and
                ($candidate -match ('case {0}:\s*goto loc_{1:X8};' -f $caseIndex,$spec.Labels[$caseIndex]))
        }
        $matchesTable
    })
    # The match setup also has a separate four-case switch using a different index.
    if ($dispatches.Count -ne 1) { throw "Missing or ambiguous complete switch: $name" }
    $cases = $dispatches[0].Groups['cases'].Value
    for ($index=0; $index -lt $spec.Count; ++$index) {
        $target = $spec.Labels[$index]
        if ($cases -notmatch ('case {0}:\s*goto loc_{1:X8};' -f $index,$target) -or
            $body -notmatch ('(?m)^loc_{0:X8}:' -f $target)) {
            throw "Invalid switch route $index in $name"
        }
    }
    $body = $body.Replace("PPC_FUNC_IMPL(__imp__$name)","PPC_FUNC_IMPL($name)")
    $overlay = Join-Path $output "$name.cpp"
    [IO.File]::WriteAllText($overlay, "// Private generated overlay. Do not distribute.`n#include `"ppc_recomp_shared.h`"`n$body`n")
    $records += [ordered]@{ function=$name; table=('0x{0:X8}' -f $spec.Table);
        entries=$spec.Count; unique_destinations=@($spec.Labels | Sort-Object -Unique).Count;
        routes_validated=$spec.Count; overlay_sha256=(Get-FileHash $overlay -Algorithm SHA256).Hash }
}
$manifest = [ordered]@{ schema=1; method='private-image switch extraction and generated route validation';
    image_sha256=(Get-FileHash $imagePath -Algorithm SHA256).Hash;
    xex_sha256=(Get-FileHash (Join-Path $root 'game/extracted/default.xex') -Algorithm SHA256).Hash;
    generator_sha256=(Get-FileHash $generator -Algorithm SHA256).Hash;
    original_switches_sha256=(Get-FileHash $switchPath -Algorithm SHA256).Hash;
    generated_config_sha256=(Get-FileHash $configPath -Algorithm SHA256).Hash;
    functions=$records; validation='static generated route checks; execution tests pending' }
$manifest | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'manifest.json') -Encoding utf8
$records | ForEach-Object { '{0}: {1} table routes validated' -f $_.function,$_.entries }
