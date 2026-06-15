# Etapa 3 — Job system fork-join (per-entity build) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Parallelize the per-entity render build (Animation/bone/skin/collect) across a thread pool to remove the single-thread bottleneck, isolating shared scratch per-worker and the durable render pose per-entity — behind `MU_JOBS`, parallelism turned on last.

**Architecture:** Fork-join within the frame. The character pass splits into 3 phases: G (cull → flat visible list, serial), B (per-entity build, parallel workers), F (merge per-worker output → InstFlush, serial, owns GL). State isolation is the load-bearing refactor: the ~30 MB per-vertex scratch becomes a per-worker thread-local arena; the bone palette is built into the per-entity `OBJECT->BoneTransform`; instance buckets are per-worker, merged order-independently at flush.

**Tech Stack:** C++20, SDL/OpenGL fixed-function + existing instanced BMD shaders, doctest unit tests (`mu_add_test`), CMake/Ninja (`windows-x86-release`), env-flag gating (`MU_*`).

**Spec:** `docs/superpowers/specs/2026-06-15-etapa3-job-system-design.md`

---

## Reference: build & test commands

- **Build client (Release):** `cmd.exe /c "I:\MuOnline\MuMain-temporal\build-rel.bat"` (vcvars x86 VS18/Insiders + cmake `--build --preset windows-x86-release`).
- **Build + run unit tests:** doctest tree is gated by `BUILD_TESTING`. Configure a test build once, then build the new module and run its case:
  ```
  cmd.exe /c "call \"C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat\" x86 && cd /d I:\MuOnline\MuMain-temporal && cmake -S . -B out/build/tests -G Ninja -DBUILD_TESTING=ON && cmake --build out/build/tests --target <test_name> && ctest --test-dir out/build/tests -R <test_name> --output-on-failure"
  ```
  (`<test_name>` is the `NAME` passed to `mu_add_test`.)
- **A/B perf harness:** `cmd.exe /c "I:\MuOnline\MuMain-temporal\run-harness-ab.bat <blendinst> <waveinst> <tag>"` already exists; this plan adds a `MU_JOBS` env to a sibling harness (Task 7's verify). Stats land in `out/build/windows-x86/src/Release/gl_log_<tag>.txt` (`[bmd_cov]`, `[frame]`, new `[jobs]`).
- **In-game:** `cmd.exe /c "I:\MuOnline\MuMain-temporal\run-ingame-14.bat 1 1"` (server must be up: `MUnique.OpenMU.Startup.exe -autostart -resolveIP:loopback`, NEVER `-reinit`).

## Reference: invariants (do not break)

- `sizeof(BMD)` unchanged (array `Models` with offset → crash login; `crash-login-scene-resuelto`).
- Every task except the last is **serial-identical**: flag OFF (and even flag ON before Task 7) must produce pixel-identical output and equal `[bmd_cov]` counts vs the previous commit.
- Phase B invariant (enforced from Task 7 on): touches only per-entity-owned data (one entity per worker), per-worker arenas/buckets, and read-only shared data. **No write to a shared mutable global.**

---

### Task 1: `Core::Jobs::ThreadPool` + `ParallelFor`

**Goal:** A minimal persistent fork-join thread pool with a blocking `ParallelFor`, plus the `MU_JOBS` flag accessor. No engine deps, unit-tested.

**Files:**
- Create: `src/source/Core/Jobs/ThreadPool.h`
- Create: `src/source/Core/Jobs/ThreadPool.cpp`
- Create: `tests/jobs/CMakeLists.txt`
- Create: `tests/jobs/test_thread_pool.cpp`
- Modify: `tests/CMakeLists.txt` (add `add_subdirectory(jobs)`)
- Modify: `src/CMakeLists.txt` (add the two new source files to the `MuClient` library target — locate the `Core/Time/SimulationClock.cpp` entry and add the `Core/Jobs/ThreadPool.cpp` next to it)

**Acceptance Criteria:**
- [ ] `ParallelFor(n, fn)` invokes `fn(i)` exactly once for every `i` in `[0,n)` and blocks until all complete.
- [ ] Worker count = `min(16, hardware_concurrency()-2)`, clamped to `>=1`.
- [ ] `n == 0` is a no-op; `n < workerCount` runs only `n` invocations.
- [ ] An exception thrown in one `fn(i)` is captured and rethrown from `ParallelFor` after all work drains (no crash, no deadlock).
- [ ] `JobsEnabled()` reads `MU_JOBS` (`=1` on, default OFF) via the same getenv_s/atoi pattern as `GpuBlendInstEnabled`.
- [ ] `CurrentWorkerIndex()` returns a stable `[0,workerCount)` id inside `fn`, and `0` on the main thread outside `ParallelFor`.

**Verify:** `ctest --test-dir out/build/tests -R test_thread_pool --output-on-failure` → all cases pass.

**Steps:**

- [ ] **Step 1: Write the failing test** — `tests/jobs/test_thread_pool.cpp`

```cpp
#include "doctest.h"

#include "Core/Jobs/ThreadPool.h"

#include <atomic>
#include <vector>
#include <stdexcept>

using Core::Jobs::ThreadPool;

TEST_CASE("ParallelFor visits every index exactly once")
{
    ThreadPool& pool = ThreadPool::Instance();
    const int n = 1000;
    std::vector<std::atomic<int>> hits(n);
    for (auto& h : hits) h.store(0);

    pool.ParallelFor(n, [&](int i) { hits[i].fetch_add(1); });

    for (int i = 0; i < n; ++i)
        CHECK(hits[i].load() == 1);
}

TEST_CASE("ParallelFor with n=0 is a no-op")
{
    ThreadPool& pool = ThreadPool::Instance();
    std::atomic<int> calls{0};
    pool.ParallelFor(0, [&](int) { calls.fetch_add(1); });
    CHECK(calls.load() == 0);
}

TEST_CASE("ParallelFor blocks until all work is done")
{
    ThreadPool& pool = ThreadPool::Instance();
    const int n = 64;
    std::atomic<int> done{0};
    pool.ParallelFor(n, [&](int) { done.fetch_add(1); });
    CHECK(done.load() == n);   // already complete when ParallelFor returns
}

TEST_CASE("ParallelFor rethrows a worker exception")
{
    ThreadPool& pool = ThreadPool::Instance();
    bool threw = false;
    try {
        pool.ParallelFor(32, [](int i) { if (i == 7) throw std::runtime_error("boom"); });
    } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    // Pool still usable afterward.
    std::atomic<int> c{0};
    pool.ParallelFor(8, [&](int) { c.fetch_add(1); });
    CHECK(c.load() == 8);
}

TEST_CASE("worker count is sane")
{
    CHECK(Core::Jobs::ThreadPool::Instance().WorkerCount() >= 1);
    CHECK(Core::Jobs::ThreadPool::Instance().WorkerCount() <= 16);
}
```

- [ ] **Step 2: Module CMake** — `tests/jobs/CMakeLists.txt`

```cmake
mu_add_test(
    NAME test_thread_pool
    SOURCES
        test_thread_pool.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Core/Jobs/ThreadPool.cpp
)
```
And append to `tests/CMakeLists.txt` after `add_subdirectory(editor)`:
```cmake
add_subdirectory(jobs)
```

- [ ] **Step 3: Header** — `src/source/Core/Jobs/ThreadPool.h`

```cpp
#pragma once

#include <cstddef>
#include <functional>

namespace Core::Jobs
{
    // Minimal persistent fork-join pool. ParallelFor splits [0,count) across the
    // workers + the calling thread and blocks until every index has run. One pool
    // per process. No engine deps -> unit-testable.
    class ThreadPool
    {
    public:
        static ThreadPool& Instance();

        // Run fn(i) for each i in [0,count). Blocks until all complete. An
        // exception from any fn is captured and rethrown after the batch drains.
        void ParallelFor(int count, const std::function<void(int)>& fn);

        int WorkerCount() const { return m_workerCount; }

        // Stable [0,WorkerCount()) id for the calling thread inside ParallelFor;
        // 0 on the main thread outside it. Used to index per-worker arenas/buckets.
        static int CurrentWorkerIndex();

    private:
        ThreadPool();
        ~ThreadPool();
        ThreadPool(const ThreadPool&) = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        int m_workerCount = 1;
        struct Impl;
        Impl* m_impl = nullptr;
    };

    // MU_JOBS env flag (default OFF). Gates the parallel Phase B (Task 7).
    bool JobsEnabled();
    void SetJobsEnabled(bool on);
}
```

- [ ] **Step 4: Implementation** — `src/source/Core/Jobs/ThreadPool.cpp`

```cpp
#include "Core/Jobs/ThreadPool.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace Core::Jobs
{
    namespace
    {
        thread_local int t_workerIndex = 0;   // main thread = 0

        int ComputeWorkerCount()
        {
            int hc = (int)std::thread::hardware_concurrency();
            int n  = std::max(1, std::min(16, hc - 2));
            return n;
        }
    }

    struct ThreadPool::Impl
    {
        std::vector<std::thread> threads;          // worker count - 1 (main also works)
        std::mutex               mtx;
        std::condition_variable  cvStart;
        std::condition_variable  cvDone;
        const std::function<void(int)>* job = nullptr;
        int   count = 0;
        std::atomic<int> nextIndex{0};
        std::atomic<int> remaining{0};
        int   epoch = 0;                           // bumped per batch to wake workers
        bool  stop = false;
        std::exception_ptr error;                  // first captured worker exception
    };

    ThreadPool& ThreadPool::Instance()
    {
        static ThreadPool s_pool;
        return s_pool;
    }

    ThreadPool::ThreadPool()
    {
        m_workerCount = ComputeWorkerCount();
        m_impl = new Impl();
        // Spawn (m_workerCount - 1) background threads; the calling thread is worker 0.
        for (int w = 1; w < m_workerCount; ++w)
        {
            m_impl->threads.emplace_back([this, w] {
                t_workerIndex = w;
                Impl& im = *m_impl;
                int seenEpoch = 0;
                for (;;)
                {
                    std::unique_lock<std::mutex> lk(im.mtx);
                    im.cvStart.wait(lk, [&] { return im.stop || im.epoch != seenEpoch; });
                    if (im.stop) return;
                    seenEpoch = im.epoch;
                    lk.unlock();

                    for (;;)
                    {
                        int i = im.nextIndex.fetch_add(1);
                        if (i >= im.count) break;
                        try { (*im.job)(i); }
                        catch (...) {
                            std::lock_guard<std::mutex> g(im.mtx);
                            if (!im.error) im.error = std::current_exception();
                        }
                        if (im.remaining.fetch_sub(1) == 1) im.cvDone.notify_all();
                    }
                }
            });
        }
    }

    ThreadPool::~ThreadPool()
    {
        if (m_impl)
        {
            {
                std::lock_guard<std::mutex> g(m_impl->mtx);
                m_impl->stop = true;
            }
            m_impl->cvStart.notify_all();
            for (auto& t : m_impl->threads) if (t.joinable()) t.join();
            delete m_impl;
            m_impl = nullptr;
        }
    }

    int ThreadPool::CurrentWorkerIndex() { return t_workerIndex; }

    void ThreadPool::ParallelFor(int count, const std::function<void(int)>& fn)
    {
        if (count <= 0) return;
        Impl& im = *m_impl;

        if (m_workerCount <= 1 || count == 1)
        {
            // Serial fast path (also keeps the flag-off / single-core build identical).
            for (int i = 0; i < count; ++i) fn(i);
            return;
        }

        {
            std::lock_guard<std::mutex> g(im.mtx);
            im.job = &fn;
            im.count = count;
            im.nextIndex.store(0);
            im.remaining.store(count);
            im.error = nullptr;
            ++im.epoch;
        }
        im.cvStart.notify_all();

        // The calling thread (worker 0) also pulls work.
        for (;;)
        {
            int i = im.nextIndex.fetch_add(1);
            if (i >= count) break;
            try { fn(i); }
            catch (...) {
                std::lock_guard<std::mutex> g(im.mtx);
                if (!im.error) im.error = std::current_exception();
            }
            if (im.remaining.fetch_sub(1) == 1) im.cvDone.notify_all();
        }

        {
            std::unique_lock<std::mutex> lk(im.mtx);
            im.cvDone.wait(lk, [&] { return im.remaining.load() == 0; });
        }

        if (im.error) { auto e = im.error; im.error = nullptr; std::rethrow_exception(e); }
    }

    namespace { bool s_jobsEnabled = false; }

    void SetJobsEnabled(bool on) { s_jobsEnabled = on; }
    bool JobsEnabled()
    {
        static const bool s_envInit = [] {
            char b[8] = {}; size_t n = 0;
            if (getenv_s(&n, b, sizeof(b), "MU_JOBS") == 0 && n > 0)
                s_jobsEnabled = (atoi(b) != 0);
            return true;
        }();
        (void)s_envInit;
        return s_jobsEnabled;
    }
}
```
(Add `#include <atomic>` — used by Impl. Place near the other includes.)

- [ ] **Step 5: Wire into the client lib** — `src/CMakeLists.txt`: find the line adding `source/Core/Time/SimulationClock.cpp` to the `MuClient` target sources and add `source/Core/Jobs/ThreadPool.cpp` alongside it.

- [ ] **Step 6: Build + run tests**

Run: the "Build + run unit tests" command with `<test_name>` = `test_thread_pool`.
Expected: all TEST_CASEs PASS.

- [ ] **Step 7: Commit**

```bash
git add src/source/Core/Jobs/ThreadPool.h src/source/Core/Jobs/ThreadPool.cpp tests/jobs/ tests/CMakeLists.txt src/CMakeLists.txt
git commit -m "feat(jobs): minimal fork-join ThreadPool + ParallelFor + MU_JOBS flag"
```

---

### Task 2: `WorkerArena` — per-worker per-vertex scratch (serial-identical refactor)

**Goal:** Move the ~30 MB file-global per-vertex scratch (`VertexTransform`/`NormalTransform`/`IntensityTransform`/`LightTransform`/`g_chrome`) into a per-worker arena reached through a thread-local pointer, with zero behavior change (serial path uses the main-thread arena). This unblocks parallel skinning later without editing every call site.

**Files:**
- Create: `src/source/Render/Build/WorkerArena.h`
- Create: `src/source/Render/Build/WorkerArena.cpp`
- Modify: `src/source/Render/Models/ZzzBMD.cpp` (replace the global array definitions at lines 36-41 + `g_chrome` at 744 with arena-backed accessors; keep every existing `VertexTransform[i][j]`-style read site unchanged via macros)
- Modify: `src/source/Render/Models/ZzzBMD.h` (the `extern` declarations for those arrays, if present)
- Modify: `src/CMakeLists.txt` (add `WorkerArena.cpp`)
- Create: `tests/jobs/test_worker_arena.cpp` + add to `tests/jobs/CMakeLists.txt`

**Acceptance Criteria:**
- [ ] The five scratch buffers live in a `WorkerArena` allocated per worker (`ThreadPool::WorkerCount()` arenas + the main thread's = use the arena indexed by `CurrentWorkerIndex()`).
- [ ] Every existing read/write site compiles unchanged (accessor macros resolve `VertexTransform` → `Render::Build::CurrentArena().vertexTransform`, etc.).
- [ ] Two threads calling `CurrentArena()` get distinct arena instances; the main thread gets index 0.
- [ ] Client builds; in-game + harness output is **pixel-identical and `[bmd_cov]`-identical** to the pre-task commit (this is a pure relocation).

**Verify:** `ctest ... -R test_worker_arena` passes; then build Release and run `run-harness-ab.bat 1 1 arena_after`, diff `[bmd_cov]`/`[frame]` vs a `arena_before` baseline (counts identical).

**Steps:**

- [ ] **Step 1: Arena header** — `src/source/Render/Build/WorkerArena.h`

```cpp
#pragma once

#include "Render/Models/ZzzBMD.h"   // MAX_MESH, MAX_VERTICES, vec3_t

namespace Render::Build
{
    // Per-worker transient skin scratch. One instance per job worker + the main
    // thread, indexed by Core::Jobs::ThreadPool::CurrentWorkerIndex(). ~30 MB each;
    // never per-entity (per-entity would be GBs). Replaces the old file-global
    // VertexTransform/NormalTransform/IntensityTransform/LightTransform/g_chrome.
    struct WorkerArena
    {
        vec3_t vertexTransform[MAX_MESH][MAX_VERTICES];
        vec3_t normalTransform[MAX_MESH][MAX_VERTICES];
        float  intensityTransform[MAX_MESH][MAX_VERTICES];
        vec3_t lightTransform[MAX_MESH][MAX_VERTICES];
        float  chrome[MAX_VERTICES][2];
    };

    // Arena for the calling thread. Heap-allocated lazily, kept for process life.
    WorkerArena& CurrentArena();

    // Pre-allocate all worker arenas (call once at startup; avoids first-frame stalls).
    void InitArenas(int count);
}
```

- [ ] **Step 2: Arena impl** — `src/source/Render/Build/WorkerArena.cpp`

```cpp
#include "Render/Build/WorkerArena.h"

#include "Core/Jobs/ThreadPool.h"

#include <memory>
#include <vector>

namespace Render::Build
{
    namespace
    {
        // Indexed by worker id. Heap (each ~30 MB) so we don't blow the stack/BSS.
        std::vector<std::unique_ptr<WorkerArena>> s_arenas;

        WorkerArena& ArenaAt(int idx)
        {
            if ((int)s_arenas.size() <= idx) s_arenas.resize(idx + 1);
            if (!s_arenas[idx]) s_arenas[idx] = std::make_unique<WorkerArena>();
            return *s_arenas[idx];
        }
    }

    void InitArenas(int count)
    {
        for (int i = 0; i < count; ++i) (void)ArenaAt(i);
    }

    WorkerArena& CurrentArena()
    {
        return ArenaAt(Core::Jobs::ThreadPool::CurrentWorkerIndex());
    }
}
```

- [ ] **Step 3: Replace the globals with accessor macros** — in `ZzzBMD.cpp`, delete the definitions at lines 36-41 (`BoneTransform` stays for now — handled in Task 3) and `g_chrome` at 744, and add near the top (after includes):

```cpp
#include "Render/Build/WorkerArena.h"

// Per-vertex skin scratch now lives in the per-worker arena (Task 2). These
// macros keep every existing VertexTransform[i][j] call site unchanged.
#define VertexTransform     (Render::Build::CurrentArena().vertexTransform)
#define NormalTransform     (Render::Build::CurrentArena().normalTransform)
#define IntensityTransform  (Render::Build::CurrentArena().intensityTransform)
#define LightTransform      (Render::Build::CurrentArena().lightTransform)
#define g_chrome            (Render::Build::CurrentArena().chrome)
```
Remove any matching `extern` lines in `ZzzBMD.h`. If any OTHER translation unit referenced these as `extern`, point it at the arena too (grep `extern .*VertexTransform` across `src/source`; convert each include + use). Expected sites: the CPU shadow fallback and any legacy draw helper in `ZzzBMD.cpp` itself (same file → covered by the macro).

- [ ] **Step 4: Startup pre-alloc** — call `Render::Build::InitArenas(Core::Jobs::ThreadPool::Instance().WorkerCount())` once during client init (next to other render init in `Winmain.cpp` after GL is up). Include both headers.

- [ ] **Step 5: Arena isolation test** — `tests/jobs/test_worker_arena.cpp`

```cpp
#include "doctest.h"

#include "Render/Build/WorkerArena.h"
#include "Core/Jobs/ThreadPool.h"

#include <set>
#include <mutex>

TEST_CASE("each worker gets a distinct arena; main thread is index 0")
{
    using Core::Jobs::ThreadPool;
    ThreadPool& pool = ThreadPool::Instance();

    std::set<const void*> seen;
    std::mutex m;
    pool.ParallelFor(pool.WorkerCount() * 8, [&](int) {
        const void* a = &Render::Build::CurrentArena();
        std::lock_guard<std::mutex> g(m);
        seen.insert(a);
    });
    // At most WorkerCount distinct arenas, at least 1.
    CHECK(seen.size() >= 1);
    CHECK(seen.size() <= (size_t)pool.WorkerCount());
}
```
Add `test_worker_arena` to `tests/jobs/CMakeLists.txt` (SOURCES include `WorkerArena.cpp`, `ThreadPool.cpp`).

- [ ] **Step 6: Build, test, A/B-identical check**

Run the unit test (`-R test_worker_arena`) → PASS. Build Release. Capture `run-harness-ab.bat 1 1 arena_before` BEFORE this task's code change is not possible post-hoc — instead compare against the Task 1 commit by stashing: simplest is to assert `[bmd_cov]` counts equal the values recorded in `docs/perf/07` (inst=2300, all else 0 at 100ch) and the in-game frame is visually unchanged.

- [ ] **Step 7: Commit**

```bash
git add src/source/Render/Build/ src/source/Render/Models/ZzzBMD.cpp src/source/Render/Models/ZzzBMD.h src/source/App/Platform/Windows/Winmain.cpp src/CMakeLists.txt tests/jobs/
git commit -m "refactor(render): move per-vertex skin scratch to per-worker WorkerArena (serial-identical)"
```

---

### Task 3: Bone palette per-entity + reentrancy audit (serial-identical)

**Goal:** Build each entity's bone matrices into its own `OBJECT->BoneTransform` (already a `vec34_t*`) instead of the shared global `BoneTransform`, and make every other global written during the per-entity build either per-worker, read-only, or relocated to Phase G. After this, the per-entity build writes no shared mutable global.

**Files:**
- Modify: `src/source/Render/Models/ZzzBMD.cpp` (global `BoneTransform[MAX_BONES][3][4]` at line 36 → per-worker arena member used as build scratch; publish into `pObject->BoneTransform`; chrome bone caches `g_chromeage`/`g_chromeup`/`g_chromeright` at 744-747)
- Modify: `src/source/Render/Build/WorkerArena.h` (add `boneScratch`, `chromeAge`, `chromeUp`, `chromeRight`)
- Modify: `src/source/Engine/Object/ZzzCharacter.cpp` (cloaking block that writes `Hero->TargetCharacter`, lines ~11354-11395 → move target-clear decision to Phase G in Task 6; for now, extract into a helper `ClearCloakedTarget(c)` called from the serial loop so Task 6 can relocate it)
- Modify: `src/source/Engine/Object/ZzzObject.cpp` (`g_smodels_total` and any other build-time global — audit)

**Acceptance Criteria:**
- [ ] `grep -rnE '\b(BoneTransform|g_chrome(age|up|right)|g_smodels_total)\b\s*\[' src/source` shows no write to a *file-global* during the per-entity build path (all go through the arena or the per-object pointer).
- [ ] A documented audit list (in the commit body or a `docs/perf/08-jobs-audit.md`) enumerates every global written during `RenderCharacter`→`Transform`→`Animation` and its resolution (per-worker / per-entity / read-only / moved to Phase G).
- [ ] Build + in-game + harness pixel-identical and `[bmd_cov]`-identical to Task 2.

**Verify:** build Release, `run-harness-ab.bat 1 1 bone_after`; `[bmd_cov]` identical to Task 2; in-game multi-map visually unchanged.

**Steps:**

- [ ] **Step 1: Audit** — run and record:
```
grep -rnE 'g_smodels_total|g_chromeage|g_chromeup|g_chromeright|^float[[:space:]]+BoneTransform|TargetCharacter[[:space:]]*=' src/source/Render src/source/Engine
```
Write each hit + resolution into `docs/perf/08-jobs-audit.md`. Resolutions:
  - `BoneTransform[MAX_BONES][3][4]` (build scratch) → `WorkerArena::boneScratch[MAX_BONES][3][4]`; after the hierarchy concat, copy into `pObject->BoneTransform` (the durable per-entity palette the GPU TBO + render read).
  - `g_chromeage/up/right[MAX_BONES]` (per-bone chrome cache, keyed by `g_smodels_total`) → `WorkerArena` members (transient per build).
  - `g_smodels_total` (monotonic "frame id" used to invalidate the chrome cache) → snapshot once in Phase G into a read-only `int` passed down; do not increment inside Phase B.
  - `Hero->TargetCharacter = -1` in the cloaking block → extract to `ClearCloakedTarget(CHARACTER*)`; called serially now, relocated to Phase G in Task 6.

- [ ] **Step 2: Add arena members** — extend `WorkerArena` (Task 2 struct) with:
```cpp
        float boneScratch[MAX_BONES][3][4];
        int   chromeAge[MAX_BONES];
        vec3_t chromeUp[MAX_BONES];
        vec3_t chromeRight[MAX_BONES];
```
Add `#include "Render/Models/ZzzBMD.h"` already present (MAX_BONES). Macro-map in `ZzzBMD.cpp`:
```cpp
#define BoneTransform   (Render::Build::CurrentArena().boneScratch)
#define g_chromeage     (Render::Build::CurrentArena().chromeAge)
#define g_chromeup      (Render::Build::CurrentArena().chromeUp)
#define g_chromeright   (Render::Build::CurrentArena().chromeRight)
```
Delete the corresponding file-global definitions (lines 36, 745-747).

- [ ] **Step 3: Publish bone palette to the entity** — find where the skeleton hierarchy concat finishes filling the (now arena) `BoneTransform` for a character (the `R_ConcatTransforms(... BoneTransform[i_])` loop, ~line 581). After the loop, ensure `pObject->BoneTransform` is allocated (`MAX_BONES` `vec34_t`) and `memcpy` the arena scratch into it, OR have the concat write straight into `pObject->BoneTransform`. The downstream `InstAppendPalette`/legacy draw must read `pObject->BoneTransform` (per-entity), not the arena scratch. Grep `pObject->BoneTransform` / `o->BoneTransform` to confirm consumers already use the per-object pointer (line 396 shows they do).

- [ ] **Step 4: Snapshot `g_smodels_total`** — replace in-build increments/reads with a value captured before the build. Add a parameter or a `thread_local`-free read-only global set in Phase G (Task 6 wires Phase G; for now set it at the top of `RenderCharactersClient`).

- [ ] **Step 5: Extract `ClearCloakedTarget`** — move the `Hero->TargetCharacter = -1` logic out of the inner render decision into a named helper, called in the same place (serial) so the diff is behavior-neutral now and relocatable later.

- [ ] **Step 6: Build, A/B-identical, commit**
```bash
git add src/source/Render/Build/WorkerArena.h src/source/Render/Models/ZzzBMD.cpp src/source/Engine/Object/ZzzCharacter.cpp src/source/Engine/Object/ZzzObject.cpp docs/perf/08-jobs-audit.md
git commit -m "refactor(render): bone palette per-entity + isolate build-time globals (serial-identical, audited)"
```

---

### Task 4: Per-worker instance buckets + merge (serial-identical)

**Goal:** Make `BmdInstanceBatch` collect into per-worker bucket sets during the build and merge them order-independently at flush, plus per-worker bone-palette TBO staging concatenated with corrected offsets. Serial path uses worker 0 → identical output.

**Files:**
- Modify: `src/source/Render/Models/BmdInstanceBatch.cpp` (`s_buckets` → per-worker; `InstAdd` uses `CurrentWorkerIndex()`; new `MergeWorkerBuckets()`; `InstFlush` merges first)
- Modify: `src/source/Render/Models/BmdInstanceBatch.h` (declare merge; note worker-indexed collect)
- Modify: `src/source/Render/GL/BonePaletteTBO.*` (per-worker append segments + base-offset fixup on merge) — inspect first; if `InstAppendPalette` returns a base index, per-worker staging must rebase on concat
- Create: `tests/jobs/test_bucket_merge.cpp` + add to `tests/jobs/CMakeLists.txt`

**Acceptance Criteria:**
- [ ] During the build, `InstAdd` appends to `buckets[CurrentWorkerIndex()]`; `InstFlush` first merges all per-worker buckets into the final draw set, then issues draws exactly as today.
- [ ] Merge is order-independent for additive buckets and correct for opaque (depth-tested) — merged instance arrays = concatenation of per-worker arrays per `(model,mesh,tex,mode,blend)` key.
- [ ] Bone-palette base indices remain valid after merge (per-worker TBO segments rebased).
- [ ] Unit test: feeding the same N records split across W workers vs all on worker 0 yields identical merged buckets (same keys, same instance counts, same float payloads).
- [ ] Build + harness `[bmd_cov]` (inst/draws/instances) identical to Task 3; in-game unchanged.

**Verify:** `ctest ... -R test_bucket_merge` passes; harness `[bmd_gpu]` inst draws/instances identical to Task 3 baseline.

**Steps:**

- [ ] **Step 1: Inspect TBO** — read `BonePaletteTBO` `Begin/AppendPalette/Upload`. Determine if per-worker staging needs offset rebasing (it does if `AppendPalette` hands back a running base). Record the rebase rule.
- [ ] **Step 2: Per-worker buckets** — change `std::unordered_map<uint64_t, Bucket> s_buckets;` to `std::vector<std::unordered_map<uint64_t, Bucket>> s_workerBuckets;` sized `WorkerCount()`. `InstBegin` clears each. `InstAdd` writes to `s_workerBuckets[Core::Jobs::ThreadPool::CurrentWorkerIndex()][key]`.
- [ ] **Step 3: Merge** — add `static void MergeWorkerBuckets(std::unordered_map<uint64_t,Bucket>& out)` that, for each worker map, for each key, appends `recs` into `out[key]` (creating the key with the same model/mesh/tex/mode/blend/uvScroll on first sight). Call it at the top of `InstFlush` and draw from `out`.
- [ ] **Step 4: Merge unit test** — `tests/jobs/test_bucket_merge.cpp`: factor the merge into a free function `Render::Models::MergeBuckets(span<const Bucket-map>) -> Bucket-map` testable without GL (pure data). Test: 300 synthetic records hashed to keys, split round-robin across W maps, merged == single-map concat (assert per key: instance count + byte-equal `recs`).
- [ ] **Step 5: TBO rebase** — apply the offset fixup from Step 1 so merged `paletteBase` values point at the right TBO rows.
- [ ] **Step 6: Build, A/B-identical, commit**
```bash
git add src/source/Render/Models/BmdInstanceBatch.cpp src/source/Render/Models/BmdInstanceBatch.h src/source/Render/GL/BonePaletteTBO.* tests/jobs/
git commit -m "refactor(render): per-worker instance buckets + order-independent merge (serial-identical)"
```

---

### Task 5: Phase G/B/F restructure of the char pass (serial)

**Goal:** Restructure `RenderCharactersClient` into explicit phases — G (cull + interpolation setup + cloaked-target clear → flat `VisibleList`), B (per-entity build loop), F (existing `InstFlush`) — running serially. No parallelism yet. Pure structural change, identical output.

**Files:**
- Create: `src/source/Render/Build/VisibleList.h` (the per-frame flat list + per-entity render inputs snapshot)
- Modify: `src/source/Engine/Object/ZzzCharacter.cpp` (`RenderCharactersClient` split into `GatherVisibleChars()` → `BuildVisibleChar(entry)` → existing flush; the interpolation save/restore + cloaking + target-clear move into Phase G)
- Modify: `src/source/Scenes/MainScene.cpp` (the `InstBegin()/RenderCharactersClient()/InstFlush()` bracket stays; internal split is inside the function)

**Acceptance Criteria:**
- [ ] `RenderCharactersClient` = `GatherVisibleChars()` (serial) → loop `BuildVisibleChar()` over the list (serial) → flush. The interpolation position/anim override is computed in G and stored per-entry; B reads the entry, builds, no shared writes besides per-entity.
- [ ] `ClearCloakedTarget` and `Hero->TargetCharacter` mutation happen in G only.
- [ ] `VisibleList` holds everything B needs (entity ptr, interpolated pos, interpolated anim pose, selected flag) so B touches no global decision state.
- [ ] Build + harness `[bmd_cov]` identical to Task 4; in-game unchanged.

**Verify:** build Release; `run-harness-ab.bat 1 1 phase_after`; `[bmd_cov]`/`[frame]` counts identical to Task 4; in-game multi-map unchanged.

**Steps:**

- [ ] **Step 1: VisibleList header** — `src/source/Render/Build/VisibleList.h`:
```cpp
#pragma once
struct CHARACTER; struct OBJECT;
namespace Render::Build
{
    struct VisibleChar
    {
        CHARACTER* c;
        OBJECT*    o;
        int        index;          // CharactersClient[] slot
        float      renderPos[3];   // interpolated (Phase G)
        float      animFrame, animPriorFrame;
        unsigned short animPriorAction;
        bool       selected;
    };
}
```
(Use an existing small vector / a `static std::vector<VisibleChar>` reused per frame — no per-frame alloc once warm.)

- [ ] **Step 2: Phase G** — `GatherVisibleChars(std::vector<VisibleChar>& out)`: the current loop body up to (but not including) `RenderCharacter`, i.e. the cloaking/target-clear, `o->Live/Visible` cull, remote+pose interpolation computation. Instead of mutating `o->Position`/`o->AnimationFrame` in place, store the interpolated values in the `VisibleChar` entry. Push visible entries to `out`. Call `NoteVisibleChar()` here.
- [ ] **Step 3: Phase B** — `BuildVisibleChar(const VisibleChar& e)`: apply the interpolated pos/anim onto `e.o` (it owns it), call `RenderCharacter(e.c, e.o, e.selected)`, restore. Because each entity is unique in the list, the temporary mutate-restore on `e.o` is per-entity-safe (matters in Task 7).
- [ ] **Step 4: Rewire** — `RenderCharactersClient` becomes: `GatherVisibleChars(s_vis); for (auto& e : s_vis) BuildVisibleChar(e);`. (Flush stays in MainScene/LoginScene as today.)
- [ ] **Step 5: Build, A/B-identical, commit**
```bash
git add src/source/Render/Build/VisibleList.h src/source/Engine/Object/ZzzCharacter.cpp
git commit -m "refactor(render): split char pass into Gather/Build/Flush phases (serial, identical)"
```

---

### Task 6: Turn on parallelism (Phase B via ParallelFor) — the win

**Goal:** Run Phase B (`BuildVisibleChar` over the visible list) through `ThreadPool::ParallelFor` when `MU_JOBS` is on, with a per-entity partition. Add the `[jobs]` profiling line. This is where the bottleneck is removed.

**Files:**
- Modify: `src/source/Engine/Object/ZzzCharacter.cpp` (`RenderCharactersClient` Phase B → `ParallelFor` under `JobsEnabled()`, with a min-entity threshold to stay serial for tiny N)
- Modify: `src/source/Render/Models/BmdGpuCache.cpp` (or a small `[jobs]` logger) — emit worker count + per-phase ms
- Modify: `src/source/Core/Utilities/FrameProfiler.*` if a new timed segment is needed for G/B/F

**Acceptance Criteria:**
- [ ] `MU_JOBS=1`: Phase B runs across workers; `MU_JOBS=0`: identical serial path (and `ParallelFor` serial fast-path keeps single-core identical).
- [ ] Output is pixel-identical and `[bmd_cov]`-identical to Task 5 (serial), with `MU_JOBS` on AND off.
- [ ] A `[jobs]` log line reports worker count + Phase G/B/F ms.
- [ ] Phase B partition gives each entity to exactly one worker (`ParallelFor(visible.size(), [&](int i){ BuildVisibleChar(s_vis[i]); })`); below a threshold (e.g. `< 2*WorkerCount`) it stays serial.
- [ ] Measured: at 100ch and 200ch, Phase B wall-time drops materially vs Task 5 (target: scales toward ÷WorkerCount minus overhead); `[frame] chars` down; no GL on worker threads.

**Verify:** `run-harness-ab.bat`-style with `MU_JOBS` 0 vs 1 at 100ch + 200ch; compare `[jobs]` Phase B ms (parallel << serial) and confirm `[bmd_cov]` identical; in-game multi-map pixel-identical.

**Steps:**

- [ ] **Step 1: Parallelize Phase B**
```cpp
auto& pool = Core::Jobs::ThreadPool::Instance();
const int n = (int)s_vis.size();
if (Core::Jobs::JobsEnabled() && n >= 2 * pool.WorkerCount())
    pool.ParallelFor(n, [&](int i) { BuildVisibleChar(s_vis[i]); });
else
    for (int i = 0; i < n; ++i) BuildVisibleChar(s_vis[i]);
```
- [ ] **Step 2: `[jobs]` profiling** — time G, B, F with the existing high-res clock; log `Render::GL::Log("[jobs] workers=%d gather=%.2f build=%.2f flush=%.2f", pool.WorkerCount(), gMs, bMs, fMs)` every ~30 frames (mirror `LogAndResetGpuStats`).
- [ ] **Step 3: Race audit pass** — re-read every function reachable from `BuildVisibleChar`→`RenderCharacter` and confirm the Phase B invariant (no shared mutable global write). Cross-check against `docs/perf/08-jobs-audit.md`. Fix any straggler (move to G, make per-worker, or make read-only). If TSan is available on this toolchain, run a short instrumented session.
- [ ] **Step 4: Build + A/B (off vs on) + perf**
Run harness `MU_JOBS=0` (tag jobs_off) and `MU_JOBS=1` (tag jobs_on) at 100ch + 200ch. Expected: `[bmd_cov]` identical; `[jobs] build` ms parallel ≈ serial/Workers + overhead; `[frame] chars` lower with jobs on.
- [ ] **Step 5: In-game validation** — `run-ingame-14.bat 1 1` with `MU_JOBS=1`; multi-map; confirm pixel-identical + no stutter/crash.
- [ ] **Step 6: Commit**
```bash
git add src/source/Engine/Object/ZzzCharacter.cpp src/source/Render/Models/BmdGpuCache.cpp src/source/Core/Utilities/FrameProfiler.h
git commit -m "feat(jobs): parallelize per-entity char build via ParallelFor (MU_JOBS), [jobs] profiling"
```

---

### Task 7: Extend to the objects pass (props) — optional, if ROI remains

**Goal:** Apply the same G/B/F + ParallelFor structure to `RenderObjects`/`RenderObjects_AfterCharacter` (map props share `BMD::Transform`/`RenderMesh`), reusing the arena/bucket/pool infra.

**Files:**
- Modify: `src/source/Engine/Object/ZzzObject.cpp` (`RenderObjects` at 3278, `RenderObjects_AfterCharacter` at 3496)

**Acceptance Criteria:**
- [ ] Objects pass build runs through `ParallelFor` under `MU_JOBS`, per-entity partition, same invariant.
- [ ] `[bmd_cov]`/visual identical off vs on; `[frame] objects` down at prop-dense maps.
- [ ] If measured ROI is negligible (objects ~ small after Etapa 1 work), STOP and document that instead of forcing it.

**Verify:** harness at a prop-dense map; `[frame] objects` parallel < serial; visual identical.

**Steps:**

- [ ] **Step 1: Gather props** into a visible list (mirror Task 5 for objects; props have no interpolation/cloaking → simpler).
- [ ] **Step 2: Parallelize** the build loop (mirror Task 6 Step 1).
- [ ] **Step 3: Measure**; if ROI < ~1ms, revert the parallel call (keep the serial G/B/F structure) and note it.
- [ ] **Step 4: Commit**
```bash
git add src/source/Engine/Object/ZzzObject.cpp
git commit -m "feat(jobs): parallelize objects-pass build (MU_JOBS) [or: document negligible ROI]"
```

---

## Self-review notes

- **Spec coverage:** 3.1 infra → Tasks 1–2; 3.2 isolate scratch/bone + audit → Tasks 2–3; 3.3 per-worker collect → Task 4; phase restructure → Task 5; 3.4 turn on parallel → Task 6; 3.5 objects → Task 7. FPS cap (3.6) is a spec no-goal (deferred) — intentionally no task.
- **Flag:** `MU_JOBS` default OFF; Tasks 2–5 are serial-identical regardless of flag; parallelism appears only in Tasks 6–7.
- **Risk concentration:** the only data-race surface is Task 6 Step 3 (and Task 7), gated by the audit from Task 3. Everything before is a behavior-neutral refactor verifiable by `[bmd_cov]` equality.
- **Type consistency:** `CurrentWorkerIndex()` (Task 1) indexes arenas (Task 2) and buckets (Task 4); `VisibleChar` (Task 5) is consumed by `BuildVisibleChar` (Tasks 5–6); `MergeBuckets`/`MergeWorkerBuckets` naming fixed in Task 4.
