#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>

// Convierte un cuadro NV12 a colores BGR y lo guarda como BMP
bool SaveFrameAsBmp(const wchar_t* path, const BYTE* frame, LONG stride, UINT32 width, UINT32 height)
{
    const UINT32 rowSize = (width * 3 + 3) & ~3u;
    std::vector<BYTE> pixels(static_cast<size_t>(rowSize) * height);

    const auto toByte = [](int value) { return static_cast<BYTE>(std::clamp(value >> 8, 0, 255)); };
    const BYTE* uvPlane = frame + stride * static_cast<LONG>(height);

    for (UINT32 y = 0; y < height; y++)
    {
        // Las filas de un BMP se guardan de abajo hacia arriba
        BYTE* row = &pixels[static_cast<size_t>(height - 1 - y) * rowSize];
        const BYTE* yRow = frame + stride * static_cast<LONG>(y);
        const BYTE* uvRow = uvPlane + stride * static_cast<LONG>(y / 2);

        for (UINT32 x = 0; x < width; x++)
        {
            const int c = yRow[x] - 16;
            const int d = uvRow[x & ~1u] - 128;
            const int e = uvRow[(x & ~1u) + 1] - 128;
            row[x * 3 + 0] = toByte(298 * c + 516 * d + 128);
            row[x * 3 + 1] = toByte(298 * c - 100 * d - 208 * e + 128);
            row[x * 3 + 2] = toByte(298 * c + 409 * e + 128);
        }
    }

    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 24;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixels.size());

    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42; // "BM"
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + info.biSizeImage;

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file)
    {
        return false;
    }
    fwrite(&header, sizeof(header), 1, file);
    fwrite(&info, sizeof(info), 1, file);
    fwrite(pixels.data(), 1, pixels.size(), file);
    fclose(file);
    return true;
}

