#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ppc_recomp_shared.h"
#include "kernel_imports.h"

namespace
{
constexpr uint32_t kSuccess = 0;
constexpr uint32_t kInvalidParameter = 0xC000000Du;
constexpr uint32_t kNoSuchFile = 0xC000000Fu;
constexpr uint32_t kInvalidHandle = 0xC0000008u;
constexpr uint32_t kFileHandleBase = 0x10000;

struct HostFile { FILE* stream{}; std::filesystem::path path; uint64_t size{}; };
std::mutex gFileMutex;
uint32_t gNextFileHandle = kFileHandleBase;
std::unordered_map<uint32_t, HostFile> gFiles;

std::string ReadAnsiString(uint8_t* base, uint32_t stringAddress)
{
    if (!stringAddress) return {};
    const uint16_t length = PPC_LOAD_U16(stringAddress);
    const uint32_t pointer = PPC_LOAD_U32(stringAddress + 4);
    std::string value;
    value.reserve(length);
    for (uint16_t i = 0; pointer && i < length; ++i) value.push_back(char(PPC_LOAD_U8(pointer + i)));
    return value;
}

std::filesystem::path TranslatePath(std::string xboxPath)
{
    for (char& c : xboxPath) if (c == '\\') c = '/';
    std::string lower = xboxPath;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    const char* prefixes[] = {"\\??/", "game:/", "d:/", "dvd:/", "/device/cdrom0/"};
    bool stripped;
    do {
        stripped = false;
        for (const char* prefix : prefixes) {
            const size_t length = std::strlen(prefix);
            if (lower.rfind(prefix, 0) == 0) {
                xboxPath.erase(0, length); lower.erase(0, length); stripped = true; break;
            }
        }
    } while (stripped);
    while (!xboxPath.empty() && xboxPath.front() == '/') xboxPath.erase(xboxPath.begin());
    std::filesystem::path relative(xboxPath);
    for (const auto& part : relative) if (part == "..") return {};
    return std::filesystem::path("game/extracted") / relative;
}

uint32_t OpenFile(PPCContext& ctx, uint8_t* base, uint32_t objectAttributes, uint32_t ioStatus)
{
    if (!ctx.r3.u32 || !objectAttributes) return kInvalidParameter;
    const uint32_t nameAddress = PPC_LOAD_U32(objectAttributes + 4);
    const std::string xboxPath = ReadAnsiString(base, nameAddress);
    const auto hostPath = TranslatePath(xboxPath);
    std::fprintf(stderr, "RRXX file: %s -> %s\n", xboxPath.c_str(), hostPath.string().c_str());
    if (hostPath.empty()) return kNoSuchFile;

    FILE* stream = nullptr;
    if (fopen_s(&stream, hostPath.string().c_str(), "rb") != 0 || !stream)
    {
        if (ioStatus) { PPC_STORE_U32(ioStatus, kNoSuchFile); PPC_STORE_U32(ioStatus + 4, 0); }
        return kNoSuchFile;
    }
    _fseeki64(stream, 0, SEEK_END);
    const uint64_t size = _ftelli64(stream);
    _fseeki64(stream, 0, SEEK_SET);
    uint32_t handle;
    {
        std::lock_guard lock(gFileMutex);
        handle = gNextFileHandle++;
        gFiles.emplace(handle, HostFile{stream, hostPath, size});
    }
    PPC_STORE_U32(ctx.r3.u32, handle);
    if (ioStatus) { PPC_STORE_U32(ioStatus, kSuccess); PPC_STORE_U32(ioStatus + 4, 1); }
    return kSuccess;
}
}

bool CloseGuestFile(unsigned int handle)
{
    std::lock_guard lock(gFileMutex);
    const auto file = gFiles.find(handle);
    if (file == gFiles.end()) return false;
    if (file->second.stream) std::fclose(file->second.stream);
    gFiles.erase(file);
    return true;
}

PPC_FUNC(__imp__NtCreateFile) { ctx.r3.u64 = OpenFile(ctx, base, ctx.r5.u32, ctx.r6.u32); }
PPC_FUNC(__imp__NtOpenFile) { ctx.r3.u64 = OpenFile(ctx, base, ctx.r5.u32, ctx.r6.u32); }

PPC_FUNC(__imp__NtReadFile)
{
    std::lock_guard lock(gFileMutex);
    const auto file = gFiles.find(ctx.r3.u32);
    if (file == gFiles.end()) { ctx.r3.u64 = kInvalidHandle; return; }
    if (ctx.r10.u32) _fseeki64(file->second.stream, int64_t(PPC_LOAD_U64(ctx.r10.u32)), SEEK_SET);
    const size_t read = std::fread(base + ctx.r8.u32, 1, ctx.r9.u32, file->second.stream);
    if (ctx.r7.u32) { PPC_STORE_U32(ctx.r7.u32, kSuccess); PPC_STORE_U32(ctx.r7.u32 + 4, uint32_t(read)); }
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    const std::string xboxPath = ReadAnsiString(base, PPC_LOAD_U32(ctx.r3.u32 + 4));
    const auto path = TranslatePath(xboxPath);
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(path, error);
    if (error) { ctx.r3.u64 = kNoSuchFile; return; }
    for (uint32_t i = 0; i < 0x38; i += 4) PPC_STORE_U32(ctx.r4.u32 + i, 0);
    PPC_STORE_U64(ctx.r4.u32 + 0x20, (size + 0xFFF) & ~uint64_t(0xFFF));
    PPC_STORE_U64(ctx.r4.u32 + 0x28, size);
    PPC_STORE_U32(ctx.r4.u32 + 0x30, 0x80);
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtQueryInformationFile)
{
    std::lock_guard lock(gFileMutex);
    const auto file = gFiles.find(ctx.r3.u32);
    if (file == gFiles.end()) { ctx.r3.u64 = kInvalidHandle; return; }
    if (ctx.r6.u32) std::memset(base + ctx.r5.u32, 0, ctx.r6.u32);
    if (ctx.r6.u32 >= 8) PPC_STORE_U64(ctx.r5.u32, file->second.size);
    if (ctx.r4.u32) { PPC_STORE_U32(ctx.r4.u32, kSuccess); PPC_STORE_U32(ctx.r4.u32 + 4, ctx.r6.u32); }
    ctx.r3.u64 = kSuccess;
}

PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    if (ctx.r6.u32) std::memset(base + ctx.r5.u32, 0, ctx.r6.u32);
    if (ctx.r4.u32) { PPC_STORE_U32(ctx.r4.u32, kSuccess); PPC_STORE_U32(ctx.r4.u32 + 4, ctx.r6.u32); }
    ctx.r3.u64 = kSuccess;
}
PPC_FUNC(__imp__NtFlushBuffersFile) { ctx.r3.u64 = kSuccess; }
PPC_FUNC(__imp__FscSetCacheElementCount) { ctx.r3.u64 = kSuccess; }
