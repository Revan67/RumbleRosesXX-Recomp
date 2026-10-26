param([string]$OutputDirectory='build/research/nonlocal-boundaries')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$out=[IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if(!$out.StartsWith((Join-Path $root 'build')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Output must be under build/.'}
New-Item -ItemType Directory -Force $out | Out-Null
$records=@()
foreach($unit in Get-ChildItem (Join-Path $root 'generated/ppc/ppc_recomp.*.cpp')) {
    $text=[IO.File]::ReadAllText($unit.FullName)
    foreach($fn in [regex]::Matches($text,'(?ms)^PPC_FUNC_IMPL\(__imp__(sub_[A-F0-9]+)\) \{.*?^\}')) {
        if(!$fn.Value.Contains('sub_824726C0(ctx, base);')){continue}
        $name=$fn.Groups[1].Value
        $save=[regex]::Matches($fn.Value,'ctx.lr = 0x([A-F0-9]+);\s*sub_824726C0\(ctx, base\);')
        if($save.Count -ne 1 -or $fn.Value -match '// ERROR'){throw "Unsupported save-site shape: $name"}
        $continuation=$save[0].Groups[1].Value
        $inner=$fn.Value.Substring($fn.Value.IndexOf('{')+1)
        $inner=$inner.Substring(0,$inner.LastIndexOf('}'))
        $prefix='^\s*PPC_FUNC_PROLOGUE\(\);\s*uint32_t ea\{\};'
        if($inner -notmatch $prefix){throw "Unexpected local declaration shape: $name"}
        $inner=[regex]::Replace($inner,$prefix,'')
        $replacement="ctx.lr = 0x$continuation;`nframe.Capture(ctx.r3.u32,0x$continuation,ctx.r1.u32);`nsub_824726C0(ctx, base);`nrrxx_resume_${continuation}:"
        $inner=$inner.Replace($save[0].Value,$replacement)
        $overlay="// Private generated continuation overlay. Do not distribute.`n#include `"ppc_recomp_shared.h`"`n#include `"nonlocal_transfer.h`"`nPPC_FUNC_IMPL($name) {`nPPC_FUNC_PROLOGUE();`nuint32_t ea{};`nrrxx::nonlocal::Frame frame;`nbool resume=false;`nfor (;;) {`ntry {`nif(resume) goto rrxx_resume_$continuation;`n$inner`nreturn;`n} catch(const rrxx::nonlocal::Transfer& transfer) {`nif(!frame.Accept(transfer)) throw;`nresume=true;`nrrxx_nonlocal_resumed(frame.buffer(),frame.continuation());`n}`n}`n}`n"
        $path=Join-Path $out "$name.cpp"
        $originalInstructions=@([regex]::Matches($fn.Value,'(?m)^\s*//[^\r\n]*')|ForEach-Object {$_.Value.Trim()})
        $overlayInstructions=@([regex]::Matches($inner,'(?m)^\s*//[^\r\n]*')|ForEach-Object {$_.Value.Trim()})
        if(($originalInstructions -join "`n") -ne ($overlayInstructions -join "`n")){throw "Instruction sequence changed in $name"}
        [IO.File]::WriteAllText($path,$overlay)
        $records+=@{function=$name;continuation=$continuation;source=$unit.Name;source_sha256=(Get-FileHash $unit.FullName).Hash;overlay_sha256=(Get-FileHash $path).Hash}
    }
}
if($records.Count -ne 6){throw "Expected six save-site owners; measured $($records.Count)"}
@{schema_version=1;method='mechanical native continuation scaffolding around unchanged generated instructions';tool_sha256=(Get-FileHash $PSCommandPath).Hash;image_sha256=(Get-FileHash (Join-Path $root 'game/rrxx_image.bin')).Hash;owners=$records;validation='build and execution pending'}|ConvertTo-Json -Depth 6|Set-Content (Join-Path $out 'manifest.json') -Encoding UTF8
$records|ForEach-Object{"$($_.function): resume $($_.continuation)"}
