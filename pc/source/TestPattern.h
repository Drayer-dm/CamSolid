#pragma once

#include <windows.h>

// Dibuja en formato NV12 barras de colores y una franja blanca que se mueve
// con cada cuadro, para ver a simple vista que el video avanza.
void DrawTestPattern(BYTE* scanline0, LONG pitch, UINT32 width, UINT32 height, UINT64 frameNumber);
