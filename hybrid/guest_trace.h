#pragma once

#include <cstdint>

// Diagnostic-only, scalar metadata. Never record guest memory or save contents.
extern thread_local std::uint32_t rrxx_guest_trace_counter;
void rrxx_guest_trace_start_from_env();
extern "C" void rrxx_guest_trace_entry(const char* function_name,
                                       std::uint32_t guest_lr,
                                       std::uint32_t guest_r3) noexcept;
