// Reuse cli-libc platform ABI implementations, but expose context and omit main.
#define SL_PLATFORM_NO_MAIN 1
#include "../lib/platform/cli-libc/platform.c"

const __sl_Context* SLPlatformCliEvalMainContext(void) {
    return &gMainContext;
}
