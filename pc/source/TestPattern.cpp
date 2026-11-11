#include "TestPattern.h"

#include <algorithm>
#include <cstring>

namespace
{
    struct Color
    {
        BYTE y, u, v;
    };

    constexpr Color BAR_COLORS[] = {
        { 235, 128, 128 }, // Blanco
        { 210,  16, 146 }, // Amarillo
        { 170, 166,  16 }, // Cian
        { 145,  54,  34 }, // Verde
        { 106, 202, 222 }, // Magenta
        {  81,  90, 240 }, // Rojo
        {  41, 240, 110 }, // Azul
    };
    constexpr UINT32 BAR_COUNT = ARRAYSIZE(BAR_COLORS);
    constexpr Color STRIPE_COLOR = { 235, 128, 128 };
    constexpr UINT32 STRIPE_SPEED = 8; // Píxeles por cuadro
}

void DrawTestPattern(BYTE* scanline0, LONG pitch, UINT32 width, UINT32 height, UINT64 frameNumber)
{
    // La franja cae en columnas pares porque NV12 comparte el color entre dos píxeles
    const UINT32 stripeWidth = std::max(width / 32, 2u) & ~1u;
    const UINT32 stripeX = static_cast<UINT32>(frameNumber * STRIPE_SPEED % (width - stripeWidth)) & ~1u;

    const auto colorAt = [&](UINT32 x) -> const Color&
    {
        const bool inStripe = x >= stripeX && x < stripeX + stripeWidth;
        return inStripe ? STRIPE_COLOR : BAR_COLORS[x * BAR_COUNT / width];
    };

    // Plano Y (brillo): se dibuja la primera fila y se copia a las demás
    for (UINT32 x = 0; x < width; x++)
    {
        scanline0[x] = colorAt(x).y;
    }
    for (UINT32 y = 1; y < height; y++)
    {
        memcpy(scanline0 + y * pitch, scanline0, width);
    }

    // Plano UV (color): tiene la mitad de filas y va justo después del plano Y
    BYTE* uvPlane = scanline0 + height * pitch;
    for (UINT32 x = 0; x + 1 < width; x += 2)
    {
        const Color& color = colorAt(x);
        uvPlane[x] = color.u;
        uvPlane[x + 1] = color.v;
    }
    for (UINT32 y = 1; y < height / 2; y++)
    {
        memcpy(uvPlane + y * pitch, uvPlane, width);
    }
}
