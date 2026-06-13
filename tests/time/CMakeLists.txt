mu_add_test(
    NAME test_simulation_clock
    SOURCES
        test_simulation_clock.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Core/Time/SimulationClock.cpp
)

mu_add_test(
    NAME test_fixedstep_drive
    SOURCES
        test_fixedstep_drive.cpp
        ${CMAKE_SOURCE_DIR}/src/source/Core/Time/SimulationClock.cpp
)
