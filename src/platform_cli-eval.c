// Reuse cli-libc platform ABI implementations, but expose context and omit main.
#define HOP_PLATFORM_NO_MAIN 1
#include "../lib/platform/cli-libc/platform.c"

const __hop_Context* H2PlatformCliEvalMainContext(void) {
    return &gMainContext;
}
