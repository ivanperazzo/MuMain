mu_add_test(
    NAME test_thread_pool
    SOURCES
        test_thread_pool.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Core/Jobs/ThreadPool.cpp
)
