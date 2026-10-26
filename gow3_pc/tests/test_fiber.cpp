#include "../gpu/shim/gow3_fiber.h"
#include "../gpu/shim/gow3_fiber_error.h"
#include <cassert>
#include <thread>
#include <array>
#include <cfenv>
#include <xmmintrin.h>
using namespace Libraries::Fiber;
static thread_local void* test_context;
// Unit-test stacks are ordinary host memory, with no GPU page tracking.
void Gow3KeepWritable(const void*, uint64_t, bool) {}
extern "C" void **runtime_thread_fiber_slot(void) { return &test_context; }
extern "C" uintptr_t runtime_fiber_resolve(const char*);
static thread_local OrbisFiber* a;
static thread_local OrbisFiber* b;
static void PS4_SYSV_ABI entry_a(u64 init,u64 run) {
    assert(init==100 && run==10);
    OrbisFiber* self=nullptr;
    assert(!sceFiberGetSelf(&self) && self==a);
    volatile u64 preserved=0x123456789abcdef0;
    assert(!sceFiberReturnToThread(11,&run));
    assert(run==20 && preserved==0x123456789abcdef0);
    assert(!sceFiberSwitch(b,30,&run));
    assert(run==50 && preserved==0x123456789abcdef0);
    assert(!sceFiberReturnToThread(51,&run));
    assert(run==60);
    for (;;) assert(!sceFiberReturnToThread(61,&run));
}
static void PS4_SYSV_ABI entry_b(u64 init,u64 run) {
    assert(init==200 && run==30);
    OrbisFiber* self=nullptr;
    assert(!sceFiberGetSelf(&self) && self==b);
    assert(!sceFiberReturnToThread(31,&run));
    assert(run==40);
    assert(!sceFiberSwitch(a,50,&run));
    for (;;) assert(!sceFiberReturnToThread(41,&run));
}
static void run_roundtrip() {
    alignas(16) std::array<u8,131072> stack_a{},stack_b{};
    OrbisFiber fa{},fb{};
    a=&fa; b=&fb;
    alignas(8) std::array<u32,32> options;
    options.fill(0x12345678);
    auto* option=reinterpret_cast<OrbisFiberOptParam*>(options.data());
    assert(sceFiberOptParamInitialize(nullptr)==ORBIS_FIBER_ERROR_NULL);
    assert(sceFiberOptParamInitialize(reinterpret_cast<OrbisFiberOptParam*>(options.data()+1))==ORBIS_FIBER_ERROR_ALIGNMENT);
    assert(!sceFiberOptParamInitialize(option));
    assert(options[0]==0xbb40e64d && options[1]==0x12345678);
    assert(!sceFiberInitialize(&fa,"A",entry_a,100,stack_a.data(),stack_a.size(),option,0x03500000));
    assert(!sceFiberInitialize(&fb,"B",entry_b,200,stack_b.data(),stack_b.size(),option,0x03500000));
    u64 value=0;
    assert(sceFiberReturnToThread(0,nullptr)==ORBIS_FIBER_ERROR_PERMISSION);
    assert(sceFiberSwitch(&fa,0,nullptr)==ORBIS_FIBER_ERROR_PERMISSION);
    std::fesetround(FE_DOWNWARD);
    const unsigned saved_csr=_mm_getcsr();
    assert(!sceFiberRun(&fa,10,&value) && value==11);
    assert(_mm_getcsr()==saved_csr && std::fegetround()==FE_DOWNWARD);
    assert(!sceFiberRun(&fa,20,&value) && value==31);
    assert(!sceFiberRun(&fb,40,&value) && value==51);
    assert(!sceFiberRun(&fa,60,&value) && value==61);
    assert(_mm_getcsr()==saved_csr && std::fegetround()==FE_DOWNWARD);
    assert(!test_context);
    OrbisFiberInfo info{};
    info.size=sizeof(info);
    assert(!sceFiberGetInfo(&fa,&info));
    assert(info.entry==entry_a && info.arg_on_initialize==100 && !strcmp(info.name,"A"));
    assert(!sceFiberRename(&fa,"renamed"));
    assert(!sceFiberGetInfo(&fa,&info) && !strcmp(info.name,"renamed"));
    assert(!sceFiberFinalize(&fa) && !sceFiberFinalize(&fb));
    assert(sceFiberRun(&fa,0,nullptr)==ORBIS_FIBER_ERROR_STATE);
    assert(sceFiberInitialize(nullptr,"x",entry_a,0,nullptr,0,nullptr,0)==ORBIS_FIBER_ERROR_NULL);
    assert(runtime_fiber_resolve("hVYD7Ou2pCQ#libSceFiber"));
}
int main() {
    run_roundtrip();
    std::thread one(run_roundtrip),two(run_roundtrip);
    one.join(); two.join();
    puts("Fiber creation, yield/resume, cross-fiber switching, FPU and thread isolation tests passed");
}
