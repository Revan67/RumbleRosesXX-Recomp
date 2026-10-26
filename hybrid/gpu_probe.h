#pragma once
#include <atomic>
#include <cstdio>
#include <memory>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/function_dispatcher.h>

// Count public service calls without reading command buffers or changing them.
class GpuProbe final : public rex::system::IGraphicsSystem {
    std::unique_ptr<rex::system::IGraphicsSystem> backend_;
    std::atomic<unsigned> rings_{0}, callbacks_{0}, writebacks_{0};
    std::atomic<uint32_t> writeback_address_{0};
    rex::memory::Memory* memory_ = nullptr;
    std::atomic<uint32_t> initial_readback_{0};
    std::atomic<uint32_t> ring_address_{0}, ring_bytes_{0};
    rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
    inline static std::atomic<PPCFunc*> callback_target_{nullptr};
    inline static std::atomic<unsigned> interrupt_entries_{0}, interrupt_returns_{0};
    uint32_t callback_address_ = 0;
    static void CountInterrupt(PPCContext& ctx, uint8_t* base) {
        ++interrupt_entries_;
        auto* target = callback_target_.load();
        if (!target) std::_Exit(24);
        target(ctx, base);
        ++interrupt_returns_;
    }
public:
    explicit GpuProbe(std::unique_ptr<rex::system::IGraphicsSystem> backend)
        : backend_(std::move(backend)) {}
    rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* context) override {
        return backend_->SetupPresentation(context);
    }
    rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
        rex::system::KernelState* kernel) override {
        memory_ = kernel->memory();
        dispatcher_ = dispatcher;
        return backend_->SetupGuestGpu(dispatcher, kernel);
    }
    bool has_presentation() const override { return backend_->has_presentation(); }
    rex::ui::GraphicsProvider* provider() const override { return backend_->provider(); }
    rex::ui::Presenter* presenter() const override { return backend_->presenter(); }
    void SetInterruptCallback(uint32_t callback, uint32_t data) override {
        ++callbacks_;
        if (callback && callback != callback_address_) {
            // This isolated probe supports one active callback address only.
            // Fail explicitly if the title needs replacement semantics.
            if (callback_address_) std::_Exit(24);
            auto* target = dispatcher_->GetFunction(callback);
            if (!target) std::_Exit(24);
            callback_target_.store(target);
            if (!dispatcher_->SetFunction(callback, CountInterrupt)) std::_Exit(24);
            callback_address_ = callback;
        }
        backend_->SetInterruptCallback(callback, data);
    }
    void InitializeRingBuffer(uint32_t ptr, uint32_t size) override {
        ring_address_.store(ptr);
        ring_bytes_.store(size < 29 ? (uint32_t(1) << (size + 3)) : 0);
        ++rings_;
        backend_->InitializeRingBuffer(ptr, size);
    }
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t size) override {
        // Observe the existing value before allowing GPU writeback; never seed it.
        if (ptr && !(ptr & 3)) {
            auto* location = memory_->TranslatePhysical(ptr);
            initial_readback_.store(_byteswap_ulong(*reinterpret_cast<volatile uint32_t*>(location)));
        }
        ++writebacks_;
        backend_->EnableReadPointerWriteBack(ptr, size);
        writeback_address_.store(ptr);
    }
    void InitializeShaderStorage(const std::filesystem::path& path,
        uint32_t title, bool blocking) override {
        backend_->InitializeShaderStorage(path, title, blocking);
    }
    void Shutdown() override { backend_->Shutdown(); }
    uint32_t writeback_address() const { return writeback_address_.load(); }
    uint32_t initial_readback() const { return initial_readback_.load(); }
    void ReportPacketSummary(uint32_t consumed) const {
        const auto bytes = ring_bytes_.load();
        if (!bytes || consumed > bytes / 4 || consumed > 128) return;
        const auto physical = ring_address_.load() & 0x1FFFFFFF;
        std::printf("Ring views: physical=%08X bytes=%u consumed=%u\n", physical, bytes, consumed);
        if (physical >= 4096) {
            auto* preceding = memory_->TranslatePhysical(physical - 4096);
            MEMORY_BASIC_INFORMATION region = {};
            if (VirtualQuery(preceding, &region, sizeof(region)) && region.State == MEM_COMMIT &&
                !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                reinterpret_cast<uintptr_t>(preceding) + consumed * 4 <=
                reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize) {
                unsigned nonzero = 0;
                for (unsigned i = 0; i < consumed; ++i)
                    if (reinterpret_cast<volatile uint32_t*>(preceding)[i]) ++nonzero;
                std::printf("Ring preceding-page check: nonzero-dwords=%u\n", nonzero);
            }
        }
        for (const uint32_t alias : {0u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
            const auto address = alias + physical;
            auto* location = memory_->TranslateVirtual(address);
            MEMORY_BASIC_INFORMATION region = {};
            if (!VirtualQuery(location, &region, sizeof(region)) || region.State != MEM_COMMIT ||
                (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                reinterpret_cast<uintptr_t>(location) + consumed * 4 >
                reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize) continue;
            unsigned nonzero = 0;
            for (unsigned i = 0; i < consumed; ++i)
                if (reinterpret_cast<volatile uint32_t*>(location)[i]) ++nonzero;
            std::printf("Ring view: guest=%08X nonzero-dwords=%u\n", address, nonzero);
        }
        auto* words = reinterpret_cast<volatile uint32_t*>(memory_->TranslatePhysical(ring_address_.load()));
        unsigned packets = 0;
        for (uint32_t offset = 0; offset < consumed && packets < 32; ++packets) {
            const auto header = _byteswap_ulong(words[offset]);
            if (!header) { ++offset; continue; } // Backend treats zero as a one-word no-op.
            const auto type = header >> 30;
            const auto payload = type == 2 ? 0u : type == 1 ? 2u : ((header >> 16) & 0x3FFF) + 1;
            if (payload > consumed - offset - 1) { std::puts("Ring summary: incomplete packet"); break; }
            std::printf("Ring summary: offset=%u type=%u payload=%u selector=%04X\n",
                offset, type, payload, type == 3 ? ((header >> 8) & 0x7F) : type == 0 ? (header & 0x7FFF) : 0);
            offset += payload + 1;
        }
    }
    void Report() const {
        std::printf("GPU interrupts: entered=%u returned=%u\n", interrupt_entries_.load(), interrupt_returns_.load());
        std::printf("GPU services: ring-init=%u interrupt-registration=%u readback-config=%u presentation=%u\n",
            rings_.load(), callbacks_.load(), writebacks_.load(), has_presentation() ? 1u : 0u);
    }
};
