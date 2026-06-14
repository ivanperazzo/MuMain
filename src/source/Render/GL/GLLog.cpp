#include "Render/GL/GLLog.h"

#include <cstdarg>
#include <cstdio>

namespace Render::GL
{
    void Log(const char* fmt, ...)
    {
        FILE* f = std::fopen("gl_log.txt", "a");
        if (f == nullptr)
            return;

        va_list args;
        va_start(args, fmt);
        std::vfprintf(f, fmt, args);
        va_end(args);

        std::fputc('\n', f);
        std::fclose(f);
    }
}
