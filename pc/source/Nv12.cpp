#include "Nv12.h"

#include <cstring>

void CopyNv12(const Nv12Frame& source, BYTE* destination, LONG pitch, UINT32 width, UINT32 height)
{
    const BYTE* sourceY = source.data.data();
    const BYTE* sourceUv = sourceY + static_cast<size_t>(source.width) * source.height;
    BYTE* destinationUv = destination + static_cast<size_t>(pitch) * height;

    // Mismo tamaño: basta con copiar fila a fila
    if (source.width == width && source.height == height)
    {
        for (UINT32 y = 0; y < height; y++)
        {
            memcpy(destination + static_cast<size_t>(pitch) * y, sourceY + static_cast<size_t>(width) * y, width);
        }
        for (UINT32 y = 0; y < height / 2; y++)
        {
            memcpy(destinationUv + static_cast<size_t>(pitch) * y, sourceUv + static_cast<size_t>(width) * y, width);
        }
        return;
    }

    // Distinto tamaño: cada píxel de destino toma el de origen más cercano
    std::vector<UINT32> columns(width);
    for (UINT32 x = 0; x < width; x++)
    {
        columns[x] = static_cast<UINT32>(static_cast<UINT64>(x) * source.width / width);
    }

    for (UINT32 y = 0; y < height; y++)
    {
        const BYTE* sourceRow = sourceY + static_cast<size_t>(source.width) * (static_cast<UINT64>(y) * source.height / height);
        BYTE* destinationRow = destination + static_cast<size_t>(pitch) * y;
        for (UINT32 x = 0; x < width; x++)
        {
            destinationRow[x] = sourceRow[columns[x]];
        }
    }

    // En el plano UV cada par de bytes (U, V) cubre dos píxeles
    for (UINT32 y = 0; y < height / 2; y++)
    {
        const UINT64 sourceRowIndex = static_cast<UINT64>(y) * (source.height / 2) / (height / 2);
        const BYTE* sourceRow = sourceUv + static_cast<size_t>(source.width) * sourceRowIndex;
        BYTE* destinationRow = destinationUv + static_cast<size_t>(pitch) * y;
        for (UINT32 x = 0; x + 1 < width; x += 2)
        {
            const UINT32 sourceX = columns[x] & ~1u;
            destinationRow[x] = sourceRow[sourceX];
            destinationRow[x + 1] = sourceRow[sourceX + 1];
        }
    }
}
