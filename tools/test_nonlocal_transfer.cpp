#include "nonlocal_transfer.h"
#include <atomic>
#include <cstdio>
#include <thread>
using rrxx::nonlocal::Frame;
using rrxx::nonlocal::Transfer;
struct Cleanup { bool& ran; ~Cleanup(){ran=true;} };
extern "C" void test_foreign_transfer(const Frame* target) { throw Transfer{target}; }
static void pass_through_nested_owner(const Frame* target) {
    Frame nested;nested.Capture(1,2,4);
    try { test_foreign_transfer(target); }
    catch(const Transfer& transfer) {
        if(nested.Accept(transfer))std::terminate();
        throw;
    }
}
int main() {
    if(Frame::Find(1,2,3))return 1;
    bool cleaned=false,caught=false;
    {
        Frame outer;outer.Capture(1,2,3);
        try {
            Frame inner;inner.Capture(4,5,6);
            Cleanup cleanup{cleaned};
            if(Frame::Find(1,2,4)||Frame::Find(9,2,3))return 2;
            pass_through_nested_owner(Frame::Find(1,2,3));
            return 3;
        } catch(const Transfer& transfer) {caught=outer.Accept(transfer);}
        if(!cleaned||!caught||Frame::Find(4,5,6))return 4;
        std::atomic<bool> isolated=false;
        std::thread thread([&]{isolated=!Frame::Find(1,2,3);Frame own;own.Capture(1,2,3);isolated=isolated&&Frame::Find(1,2,3)==&own;});
        thread.join();if(!isolated)return 5;
    }
    if(Frame::Find(1,2,3))return 6;
    std::puts("PASS: nested unwind, C-linkage exception, cleanup, unmatched/stale targets, thread isolation");
    return 0;
}
