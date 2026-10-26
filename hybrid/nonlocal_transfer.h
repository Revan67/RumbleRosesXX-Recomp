#pragma once
#include <cstdint>

namespace rrxx::nonlocal {
class Frame;
struct Transfer { const Frame* target; };

// A native frame identity, never stored in guest memory. Thread-scoped and
// stack-owned, so nested transfers unwind destructors and cannot target a
// frame that has already returned.
class Frame {
    inline static thread_local Frame* top_ = nullptr;
    Frame* previous_;
    uint32_t buffer_=0, continuation_=0, stack_=0;
    bool captured_=false;
public:
    Frame() : previous_(top_) { top_=this; }
    ~Frame() { top_=previous_; }
    Frame(const Frame&)=delete;
    Frame& operator=(const Frame&)=delete;
    void Capture(uint32_t buffer,uint32_t continuation,uint32_t stack) {
        buffer_=buffer;continuation_=continuation;stack_=stack;captured_=true;
    }
    static const Frame* Find(uint32_t buffer,uint32_t continuation,uint32_t stack) {
        for(Frame* frame=top_;frame;frame=frame->previous_) {
            if(frame->captured_ && frame->buffer_==buffer &&
               frame->continuation_==continuation && frame->stack_==stack) return frame;
        }
        return nullptr;
    }
    bool Accept(const Transfer& transfer) const { return transfer.target==this; }
    uint32_t buffer() const { return buffer_; }
    uint32_t continuation() const { return continuation_; }
};
}

void rrxx_nonlocal_resumed(uint32_t buffer,uint32_t continuation);
