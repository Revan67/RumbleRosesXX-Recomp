param(
    [Parameter(Mandatory=$true)][string]$Header,
    [Parameter(Mandatory=$true)][string]$Output
)

$names = Select-String -Path $Header -Pattern '^PPC_EXTERN_FUNC\(__imp__(.+)\);$' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
$implemented = @(
    'NtAllocateVirtualMemory', 'NtFreeVirtualMemory',
    'MmAllocatePhysicalMemoryEx', 'MmFreePhysicalMemory', 'MmGetPhysicalAddress', 'MmQueryAllocationSize', 'MmQueryAddressProtect',
    'ExAllocatePool', 'ExFreePool',
    'XamAlloc', 'XamFree',
    'KeTlsAlloc', 'KeTlsFree', 'KeTlsSetValue', 'KeTlsGetValue',
    'RtlInitializeCriticalSection', 'RtlInitializeCriticalSectionAndSpinCount',
    'RtlEnterCriticalSection', 'RtlTryEnterCriticalSection', 'RtlLeaveCriticalSection',
    'RtlInitAnsiString', 'KeQuerySystemTime', 'KeQueryPerformanceFrequency',
    'KeGetCurrentProcessType', 'XexCheckExecutablePrivilege', 'ExGetXConfigSetting',
    'NtCreateEvent', 'NtSetEvent', 'NtClearEvent', 'KeResetEvent', 'KeSetEvent',
    'KeWaitForSingleObject',
    'NtWaitForSingleObjectEx', 'NtClose', 'NtResumeThread', 'NtSuspendThread',
    'ExCreateThread', 'ObReferenceObjectByHandle', 'ObDereferenceObject',
    'RtlNtStatusToDosError', 'RtlFillMemoryUlong', 'KiApcNormalRoutineNop', 'DbgPrint',
    'KeDelayExecutionThread',
    'NtCreateFile', 'NtOpenFile', 'NtReadFile', 'NtQueryFullAttributesFile', 'NtQueryInformationFile',
    'NtQueryVolumeInformationFile', 'NtFlushBuffersFile', 'FscSetCacheElementCount',
    'XGetLanguage', 'XGetGameRegion', 'XGetVideoMode',
    'XamInputGetCapabilities', 'XamInputGetState', 'XamInputSetState',
    'DbgBreakPoint', 'ExRegisterTitleTerminateNotification',
    'NetDll_XNetStartup', 'NetDll_WSAStartup',
    'NetDll_XNetGetEthernetLinkStatus', 'NetDll_XNetGetTitleXnAddr', 'NetDll_XNetXnAddrToMachineId',
    'XamNotifyCreateListener', 'XNotifyGetNext',
    'XamUserGetSigninState', 'XamUserGetXUID', 'XamUserGetName',
    'RtlMultiByteToUnicodeN', 'XMsgInProcessCall', 'XMsgStartIORequest',
    'VdQueryVideoMode', 'VdQueryVideoFlags', 'VdGetCurrentDisplayGamma',
    'VdSetDisplayMode', 'VdGetCurrentDisplayInformation',
    'VdGetGraphicsAsicID', 'VdIsHSIOTrainingSucceeded',
    'VdInitializeEngines', 'VdShutdownEngines', 'VdSetGraphicsInterruptCallback',
    'VdSetSystemCommandBufferGpuIdentifierAddress', 'VdInitializeRingBuffer',
    'VdEnableRingBufferRPtrWriteBack', 'VdGetSystemCommandBuffer', 'VdSwap',
    'VdRetrainEDRAM', 'VdRetrainEDRAMWorker', 'VdCallGraphicsNotificationRoutines',
    'VdInitializeScalerCommandBuffer', 'VdPersistDisplay',
    'KeSetBasePriorityThread', 'KeSetAffinityThread', 'KeEnterCriticalRegion', 'KeLeaveCriticalRegion',
    'KeAcquireSpinLockAtRaisedIrql', 'KeReleaseSpinLockFromRaisedIrql',
    'KeTryToAcquireSpinLockAtRaisedIrql', 'KfAcquireSpinLock', 'KfReleaseSpinLock'
)
$names = $names | Where-Object { $_ -notin $implemented }

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('#include <cstdio>')
$lines.Add('#include "ppc_recomp_shared.h"')
$lines.Add('')
foreach ($name in $names) {
    $lines.Add("PPC_FUNC(__imp__$name) {")
    $lines.Add('    static bool reported;')
    $lines.Add(('    if (!reported) std::fprintf(stderr, "UNIMPLEMENTED XBOX IMPORT: {0}\n");' -f $name))
    $lines.Add('    reported = true;')
    $lines.Add('    ctx.r3.u64 = 0x80004001u;')
    $lines.Add('}')
    $lines.Add('')
}
[System.IO.File]::WriteAllLines($Output, $lines)
