mu_add_test(
    NAME test_thread_pool
    SOURCES
        test_thread_pool.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Core/Jobs/ThreadPool.cpp
)

mu_add_test(
    NAME test_worker_arena
    SOURCES
        test_worker_arena.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Render/Build/WorkerArena.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Core/Jobs/ThreadPool.cpp
)

mu_add_test(
    NAME test_bucket_merge
    SOURCES
        test_bucket_merge.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Render/Models/BmdInstanceBatchMerge.cpp
)
