mu_add_test(
    NAME test_anim_advance
    SOURCES
        test_anim_advance.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Render/AnimTiming.cpp
)

mu_add_test(
    NAME test_anim_interp
    SOURCES
        test_anim_interp.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Render/AnimInterp.cpp
)
