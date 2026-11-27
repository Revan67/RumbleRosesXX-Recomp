param([string]$OutputDirectory='build/research/sdk-checkpoint')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$sdk=Join-Path $root 'build/rexglue-sdk'
$out=[IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
if(!$out.StartsWith((Join-Path $root 'build')+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output must be under project build/.'
}
$baseline='c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'
function Invoke-SdkGit([string[]]$Arguments) {
    $result=@(& git -C $sdk @Arguments)
    if($LASTEXITCODE -ne 0){throw "SDK Git failed: $($Arguments -join ' ')"}
    return $result
}
if((Invoke-SdkGit @('rev-parse','HEAD')) -ne $baseline){throw 'Unexpected SDK baseline'}
$groups=[ordered]@{
 '0001-cpu-fault-observation.patch'=@('src/system/mmio_handler.cpp','src/system/xmemory.cpp')
 '0002-gpu-fault-observation.patch'=@(
  'include/rex/graphics/d3d12/command_processor.h',
  'include/rex/graphics/d3d12/draw_journal.h',
  'include/rex/ui/d3d12/d3d12_upload_buffer_pool.h','include/rex/ui/d3d12/d3d12_util.h',
  'src/graphics/d3d12/command_processor.cpp','src/graphics/d3d12/primitive_processor.cpp',
  'src/graphics/d3d12/render_target_cache.cpp','src/graphics/d3d12/shared_memory.cpp',
  'src/ui/d3d12/d3d12_presenter.cpp','src/ui/d3d12/d3d12_provider.cpp',
  'src/ui/d3d12/d3d12_upload_buffer_pool.cpp','src/ui/d3d12/d3d12_util.cpp')
 '0003-safe-relative-constants.patch'=@(
  'include/rex/graphics/pipeline/shader/dxbc_translator.h',
  'src/graphics/pipeline/shader/dxbc_translator.cpp')
}
$approved=@($groups.Values | ForEach-Object {$_})
$changed=@(Invoke-SdkGit @('diff','HEAD','--name-only','--','include','src'))
$untracked=@(Invoke-SdkGit @('ls-files','--others','--exclude-standard','--','include','src'))
foreach($path in @($changed)+@($untracked)) {
    if($path -notin $approved){throw "Unreviewed SDK source: $path"}
}
New-Item -ItemType Directory -Force $out | Out-Null
$previousIndex=[Environment]::GetEnvironmentVariable('GIT_INDEX_FILE','Process')
$index=Join-Path $out ('index-'+[guid]::NewGuid().ToString('N'))
$patches=@()
try {
    $env:GIT_INDEX_FILE=$index
    foreach($group in $groups.GetEnumerator()) {
        Invoke-SdkGit @('read-tree',$baseline) | Out-Null
        Invoke-SdkGit (@('add','--')+@($group.Value)) | Out-Null
        $patch=Join-Path $out $group.Key
        Invoke-SdkGit @('diff','--cached','--full-index','--no-ext-diff',"--output=$patch",$baseline,'--') | Out-Null
        $patches+=@{path=$group.Key;sha256=(Get-FileHash $patch).Hash;files=@($group.Value)}
    }
    # Reconstruct an index from the pristine baseline, not the dirty worktree.
    Invoke-SdkGit @('read-tree',$baseline) | Out-Null
    foreach($patch in $patches) {
        Invoke-SdkGit @('apply','--cached','--check',(Join-Path $out $patch.path)) | Out-Null
        Invoke-SdkGit @('apply','--cached',(Join-Path $out $patch.path)) | Out-Null
    }
    $files=@(foreach($path in $approved) {
        $actual=@(Invoke-SdkGit @('hash-object',"--path=$path",'--',$path))[0]
        $replayed=@(Invoke-SdkGit @('rev-parse',":$path"))[0]
        if($actual -notmatch '^[0-9a-f]{40}$' -or $replayed -notmatch '^[0-9a-f]{40}$') {
            throw "Invalid Git blob identity: $path"
        }
        if($actual -ne $replayed){throw "Patch replay differs: $path"}
        @{path=$path;git_blob=$actual;sha256=(Get-FileHash (Join-Path $sdk $path)).Hash}
    })
    $record=@{schema=1;status='staged';classification='IMPLEMENTATION / TEST RESULT';
        upstream='https://github.com/rexglue/rexglue-sdk';baseline=$baseline;
        method='Isolated Git index patch export and pristine-index replay';
        tool_sha256=(Get-FileHash $PSCommandPath).Hash;patches=$patches;files=$files;
        validation='All 16 approved source blobs reproduced from the pinned baseline';
        limitations=@('No fresh SDK compilation or gameplay performed by this tool.',
          'Submodules excluded; separately validate local submodule materialization.',
          'Patch validity does not establish semantic correctness or hardware equivalence.')}
    $record | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $out 'manifest.json') -Encoding utf8
    'PASS: three patches replayed; all 16 SDK source blobs match.'
} finally {
    if([string]::IsNullOrEmpty($previousIndex)) {
        Remove-Item Env:\GIT_INDEX_FILE -ErrorAction SilentlyContinue
    } else { $env:GIT_INDEX_FILE=$previousIndex }
    foreach($temporary in @($index,$index+'.lock')) {
        if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}
    }
}
