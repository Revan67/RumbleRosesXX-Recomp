param([string]$OutputDirectory = 'build/research/flag-boundary')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if (!$out.StartsWith((Join-Path $root 'build') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be under build/.' }
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $out 'generated') | Out-Null
$relative = [IO.Path]::GetRelativePath($out,$root).Replace('\','/')
$settings = [IO.File]::ReadAllText((Join-Path $root 'config/rrxx.toml'))
$settings = $settings.Substring($settings.IndexOf('skip_lr ='))
$config = "[main]`nfile_path = `"$relative/game/extracted/default.xex`"`nout_directory_path = `"generated`"`nswitch_table_file_path = `"$relative/config/rrxx_switch_tables.toml`"`nfunctions = [{ address = 0x8236ABE8, size = 0x9C }]`n$settings"
$configPath = Join-Path $out 'recompile.toml'
[IO.File]::WriteAllText($configPath,$config)
$generator = Join-Path $root 'tools/XenonRecomp/build/XenonRecomp/XenonRecomp.exe'
$process = Start-Process -FilePath $generator -ArgumentList @($configPath,(Join-Path $root 'tools/XenonRecomp/XenonUtils/ppc_context.h')) -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'generator.stdout.log') -RedirectStandardError (Join-Path $out 'generator.stderr.log')
try {
    if (!$process.HasExited) { $process.PriorityClass='BelowNormal'; $process.ProcessorAffinity=[intptr]3 }
    if (!$process.WaitForExit(60000)) { throw 'Generator exceeded 60 seconds' }
    if ($process.ExitCode -ne 0) { throw "Generator exit $($process.ExitCode)" }
} finally { if (!$process.HasExited) { Stop-Process -Id $process.Id } }
$bodies = @(foreach ($unit in Get-ChildItem (Join-Path $out 'generated/ppc_recomp.*.cpp')) {
    [regex]::Matches([IO.File]::ReadAllText($unit.FullName),'(?ms)^PPC_FUNC_IMPL\(__imp__sub_8236ABE8\) \{.*?^\}') | ForEach-Object Value
})
if ($bodies.Count -ne 1) { throw 'Expected one generated helper' }
$body=$bodies[0]
if ($body -match '// ERROR|PPC_CALL_INDIRECT_FUNC' -or $body -notmatch 'loc_8236AC64:' -or $body -notmatch 'ctx.r3.u64 = ctx.r3.u64 \| 64;') { throw 'Continuation is incomplete' }
$overlay=Join-Path $out 'sub_8236ABE8.cpp'
[IO.File]::WriteAllText($overlay,"// Private generated overlay. Do not distribute.`n#include `"ppc_recomp_shared.h`"`n"+$body.Replace('PPC_FUNC_IMPL(__imp__sub_8236ABE8)','PPC_FUNC_IMPL(sub_8236ABE8)')+"`n")
@{schema_version=1;method='explicit original helper extent, unchanged generator';entry='8236ABE8';end_exclusive='8236AC84';image_sha256=(Get-FileHash (Join-Path $root 'game/rrxx_image.bin')).Hash;xex_sha256=(Get-FileHash (Join-Path $root 'game/extracted/default.xex')).Hash;generator_sha256=(Get-FileHash $generator).Hash;config_sha256=(Get-FileHash $configPath).Hash;overlay_sha256=(Get-FileHash $overlay).Hash;validation='static closure check passed; isolated execution pending'} | ConvertTo-Json | Set-Content (Join-Path $out 'manifest.json') -Encoding UTF8
Write-Output 'Generated isolated helper with shared continuation; runtime not modified.'
