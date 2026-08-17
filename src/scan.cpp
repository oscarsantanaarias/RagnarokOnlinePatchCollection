#include <windows.h>
#include <psapi.h>
#include <cstring>

#include "scan.h"

#pragma comment(lib, "psapi.lib")

namespace scan {

uint8_t* image_begin = nullptr;
uint8_t* image_end   = nullptr;
uint8_t* text_begin  = nullptr;
uint8_t* text_end    = nullptr;

void Init()
{
    MODULEINFO mi{};
    HMODULE mod = GetModuleHandle(nullptr);
    GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi));

    image_begin = (uint8_t*)mi.lpBaseOfDll;
    image_end   = image_begin + mi.SizeOfImage;

    auto* dos = (IMAGE_DOS_HEADER*)image_begin;
    auto* nt  = (IMAGE_NT_HEADERS*)(image_begin + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    text_begin = image_begin + sec[0].VirtualAddress;
    text_end   = text_begin + sec[0].Misc.VirtualSize;
}

uintptr_t FindPattern(const uint8_t* pattern, const char* mask,
                      uintptr_t base, uintptr_t size)
{
    size_t len = strlen(mask);
    if (size < len) return 0;

    for (uintptr_t i = 0; i < size - len; i++) {
        bool found = true;
        for (size_t j = 0; j < len; j++) {
            if (mask[j] != '?' && pattern[j] != *(uint8_t*)(base + i + j)) {
                found = false;
                break;
            }
        }
        if (found) return base + i;
    }
    return 0;
}

uint8_t* FindBytes(uint8_t* begin, uint8_t* end, const void* needle, size_t len)
{
    if (!begin || len == 0) return nullptr;
    uint8_t first = *(const uint8_t*)needle;

    for (uint8_t* p = begin; p + len <= end; ++p)
        if (*p == first && memcmp(p, needle, len) == 0)
            return p;
    return nullptr;
}

uint8_t* FindString(const char* text)
{
    return FindBytes(image_begin, image_end, text, strlen(text) + 1);
}

uintptr_t CallTarget(const uint8_t* p)
{
    return (uintptr_t)(p + 5) + *(const int32_t*)(p + 1);
}

bool RepointCall(uint8_t* call_site, void* dest)
{
    DWORD old;
    if (!VirtualProtect(call_site, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;

    *(int32_t*)(call_site + 1) = (int32_t)((intptr_t)dest - (intptr_t)(call_site + 5));

    VirtualProtect(call_site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call_site, 5);
    return true;
}

} // namespace scan
