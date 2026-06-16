#pragma once

// File-based logging for the GPU track. The client is a GUI subsystem app, so
// fprintf(stderr) is not captured (not even under cdb) -- shader compile/link
// errors and pipeline diagnostics MUST go to a file to be analyzable. Appends to
// gl_log.txt in the working directory (the Debug dir at runtime).

namespace Render::GL
{
    // printf-style; appends a line to gl_log.txt and flushes. Cheap, off the hot
    // path (init / error reporting only).
    void Log(const char* fmt, ...);
}
