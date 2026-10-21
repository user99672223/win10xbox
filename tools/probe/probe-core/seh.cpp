// seh.cpp - structured-exception trampoline for Reporter::run.
//
// Compiled with /EHa. This translation unit intentionally contains NO C++
// objects with destructors inside the __try frame: mixing C++ unwinding and
// __try/__except in one function is ill-formed. The caller constructs the
// std::function (which has a destructor) in its own frame and passes a plain
// function pointer + void* through here, so the only thing crossing into the
// __try is a non-unwinding C call.
#include "probe.h"

namespace probe {

static int seh_filter(unsigned int code, uint32_t* out) {
    *out = static_cast<uint32_t>(code);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool run_guarded(void (*fn)(void*), void* ctx, uint32_t* code) {
    uint32_t local = 0;
    __try {
        fn(ctx);
        return true;
    } __except (seh_filter(GetExceptionCode(), &local)) {
        if (code) *code = local;
        return false;
    }
}

}  // namespace probe
