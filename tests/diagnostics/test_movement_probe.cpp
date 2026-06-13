#include "doctest.h"

#include "Core/Diagnostics/MovementProbe.h"

using Core::Diagnostics::MovementProbe;

TEST_CASE("units_per_sec is frame-rate independent")
{
    // 100 unidades en 1000 ms = 100 u/s, sin importar el numero de muestras.
    MovementProbe coarse;   // 25 muestras de 40 ms
    for (int i = 1; i <= 25; ++i)
        coarse.Sample(/*x*/ i * 4.0f, /*y*/ 0.0f, /*tMs*/ i * 40.0);

    MovementProbe fine;     // 250 muestras de 4 ms, misma trayectoria total
    for (int i = 1; i <= 250; ++i)
        fine.Sample(/*x*/ i * 0.4f, /*y*/ 0.0f, /*tMs*/ i * 4.0);

    CHECK(coarse.UnitsPerSec() == doctest::Approx(100.0).epsilon(0.01));
    CHECK(fine.UnitsPerSec()   == doctest::Approx(100.0).epsilon(0.01));
}

TEST_CASE("no movement => zero units_per_sec")
{
    MovementProbe p;
    p.Sample(10.0f, 10.0f, 40.0);
    p.Sample(10.0f, 10.0f, 80.0);
    CHECK(p.UnitsPerSec() == doctest::Approx(0.0));
}

TEST_CASE("first sample establishes origin, reports zero")
{
    MovementProbe p;
    p.Sample(5.0f, 5.0f, 0.0);
    CHECK(p.UnitsPerSec() == doctest::Approx(0.0));
}
