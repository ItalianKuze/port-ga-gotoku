// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// bb-probe (probe.c): set while the port restarts itself through run.sh. The device fd is closed
// before exec; Vulkan calls failing then are not errors: this thread waits for the exec instead.
// A function, not the variable: a variable imported across PE DLLs resolves to the import stub.
#ifdef _WIN32
// Not weak here: MinGW does not resolve a weak reference from an import library (it becomes 0).
extern "C" int runtime_is_restarting(void);
static bool restarting() { return runtime_is_restarting() != 0; }
#else
extern "C" __attribute__((weak)) int runtime_is_restarting(void); // absent in the tests
static bool restarting() { return runtime_is_restarting && runtime_is_restarting(); }
#endif

void assert_fail_impl() {
    if (restarting()) {
        for (;;) {
#ifdef _WIN32
            Sleep(INFINITE);
#else
            pause();
#endif
        }
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
