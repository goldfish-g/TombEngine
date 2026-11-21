// DirectXMath outside Windows, in the browser.
//
// The engine uses the SSE2 code path of DirectXMath (see CMakeLists.txt), which
// Emscripten runs on WebAssembly SIMD. XMVerifyCPUSupport() asks the CPU with
// cpuid, which WebAssembly has no equivalent of: it reports SSE and SSE2.

#pragma once

#ifndef __cpuid
#define __cpuid(level, a, b, c, d) \
    do { (a) = 1; (b) = 0; (c) = 0; (d) = ((level) == 1) ? 0x6000000 : 0; } while (0)
#endif

#include_next <DirectXMath.h>
