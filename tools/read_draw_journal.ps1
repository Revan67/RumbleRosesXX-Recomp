param([Parameter(Mandatory)][string]$InputPath,
      [string]$OutputPath='build/research/draw-journal-review.json',
      [string]$FaultAddress='0')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$output=[IO.Path]::GetFullPath((Join-Path $root $OutputPath))
if (!$output.StartsWith((Join-Path $root 'build')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be within build/.' }
$inputFile=Get-Item -LiteralPath $InputPath
if ($inputFile.Length -gt 1MB) { throw 'Journal exceeds expected bounded size.' }
$data=[IO.File]::ReadAllBytes($inputFile.FullName)
if ($data.Length -lt 32 -or [BitConverter]::ToUInt32($data,0) -ne 0x4A445252 -or [BitConverter]::ToUInt32($data,4) -ne 1) { throw 'Unsupported journal header.' }
$stride=[BitConverter]::ToUInt32($data,8)
$capacity=[BitConverter]::ToUInt32($data,12)
$next=[BitConverter]::ToUInt64($data,24)
if ($stride -ne 152 -or $capacity -ne 4096 -or $data.Length -ne 32+$stride*$capacity) { throw 'Unexpected journal layout.' }
$roles=@('system','float_vs','float_ps','bool_loop','fetch','descriptors_vs','descriptors_ps')
$fault=[Convert]::ToUInt64(($FaultAddress -replace '^0x',''),16)
$records=@(for ($slot=0;$slot -lt $capacity;$slot++) {
    $p=32+$slot*$stride
    $sequence=[BitConverter]::ToUInt64($data,$p)
    if ($sequence -eq [uint64]::MaxValue -or $sequence -ge $next -or $next-$sequence -gt $capacity -or $sequence%$capacity -ne $slot) { continue }
    $p+=8
    $bindings=@(for($role=0;$role -lt 7;$role++) {
        $address=[BitConverter]::ToUInt64($data,$p+56+$role*8)
        $bytes=[BitConverter]::ToUInt32($data,$p+112+$role*4)
        @{role=$roles[$role];address=('0x{0:X16}' -f $address);bytes=$bytes;
          fault_gap=if($fault -and $address -and $fault -ge $address){$fault-$address}else{$null};
          fault_within_request=($fault -and $address -and $fault -ge $address -and $fault-$address -lt $bytes)}
    })
    @{sequence=$sequence;submission=[BitConverter]::ToUInt64($data,$p);frame=[BitConverter]::ToUInt64($data,$p+8);
      index_address=('0x{0:X16}' -f [BitConverter]::ToUInt64($data,$p+16));index_bytes=[BitConverter]::ToUInt32($data,$p+24);
      index_count=[BitConverter]::ToUInt32($data,$p+28);index_type=[BitConverter]::ToUInt32($data,$p+32);
      vs=('{0:X16}' -f [BitConverter]::ToUInt64($data,$p+40));ps=('{0:X16}' -f [BitConverter]::ToUInt64($data,$p+48));
      dynamic_float_mask=[BitConverter]::ToUInt32($data,$p+140);bindings=$bindings}
})
$records=@($records | Sort-Object sequence)
$report=@{schema_version=1;status='staged';classification='IMPLEMENTATION / TEST RESULT';input_sha256=(Get-FileHash $inputFile.FullName).Hash;
    tool_sha256=(Get-FileHash $PSCommandPath).Hash;next_sequence=$next;retained_records=$records.Count;fault_address=$FaultAddress;
    limitations=@('Indexed draws only; no guest payloads or effective shader indices.','CPU enqueue records, not GPU completion or exact DRED operation attribution.','Inspect after target process exit. A last uncommitted slot can be absent.');records=$records}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $output -Encoding UTF8
Write-Output "Journal: committed=$next retained=$($records.Count) bytes=$($data.Length)"
if($records.Count) { $records[-1] | Select-Object sequence,submission,frame,vs,ps | Format-List }
