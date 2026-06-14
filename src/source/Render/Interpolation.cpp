#include "Render/Interpolation.h"

namespace Render::Interpolation
{
    void Lerp(const float a[3], const float b[3], float alpha, float out[3])
    {
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        for (int i = 0; i < 3; ++i)
            out[i] = a[i] + (b[i] - a[i]) * alpha;
    }
}
