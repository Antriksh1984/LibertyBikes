#pragma once
// Tiny inline-hook helper for gta3.exe v1.0. Prologue lengths were checked with capstone
// (tools/exe.py); `expect` guards against a different exe or another mod's patch.
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace hook {

// Replace the first `len` bytes at `addr` with a jump to `fn`. Returns a trampoline that runs
// the original function, or nullptr if the bytes are not what we expect (hook not installed).
// If another mod already put a `jmp rel32` there, we chain to its target instead.
inline void *Detour(uintptr_t addr, void *fn, size_t len, const char *expect) {
    auto *p = reinterpret_cast<uint8_t *>(addr);
    auto *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    if (p[0] == 0xE9) {
        uintptr_t target = addr + 5 + *reinterpret_cast<int32_t *>(p + 1);
        tramp[0] = 0xE9;
        *reinterpret_cast<int32_t *>(tramp + 1) = int32_t(target - (uintptr_t(tramp) + 5));
        len = 5;
    } else {
        if (expect && memcmp(p, expect, len) != 0) { VirtualFree(tramp, 0, MEM_RELEASE); return nullptr; }
        memcpy(tramp, p, len);
        tramp[len] = 0xE9;
        *reinterpret_cast<int32_t *>(tramp + len + 1) = int32_t((addr + len) - (uintptr_t(tramp) + len + 5));
    }
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9;
    *reinterpret_cast<int32_t *>(p + 1) = int32_t(uintptr_t(fn) - (addr + 5));
    for (size_t i = 5; i < len; i++) p[i] = 0x90;
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return tramp;
}

template <typename T> inline T As(void *p) { return reinterpret_cast<T>(p); }

// Overwrite bytes at addr if the current bytes match `expect`.
inline bool Patch(uintptr_t addr, const void *bytes, size_t len, const void *expect) {
    auto *p = reinterpret_cast<uint8_t *>(addr);
    if (expect && memcmp(p, expect, len) != 0) return false;
    DWORD old;
    VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, bytes, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}

} // namespace hook
