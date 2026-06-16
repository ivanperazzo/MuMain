mu_add_test(
    NAME test_motion
    SOURCES
        test_motion.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Render/Interpolation.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Motion/Integrate.cpp
)
