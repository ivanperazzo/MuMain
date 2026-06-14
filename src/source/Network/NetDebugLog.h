#pragma once

// Lightweight, debug-only network/movement tracer. Appends one line per call to
// "client_net.log" in the working directory (the client's Debug folder). Intended for
// diagnosing server-authority issues (walk resync "warps", attack-intent flow); it opens
// and closes the file per call, so it is NOT for per-frame hot paths — only for discrete
// network events (walk/attack sends, position corrections received).

#include <cstdio>
#include <cwchar>
#include <cstdarg>

inline void NetDebugLog(const wchar_t* fmt, ...)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"client_net.log", L"a+, ccs=UTF-8") != 0 || f == nullptr)
    {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);

    fputwc(L'\n', f);
    fclose(f);
}
