#pragma once

#include <windows.h>
#include <vector>

// Un cuadro NV12 compacto: el plano Y (width x height) seguido del plano UV
// (width x height / 2), sin bytes de relleno entre filas.
struct Nv12Frame
{
    std::vector<BYTE> data;
    UINT32 width = 0;
    UINT32 height = 0;
};

// Copia el cuadro a un búfer de destino con "pitch" bytes por fila. Si los
// tamaños no coinciden, lo escala tomando el píxel más cercano.
void CopyNv12(const Nv12Frame& source, BYTE* destination, LONG pitch, UINT32 width, UINT32 height);
