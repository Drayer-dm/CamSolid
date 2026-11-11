#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>

// Estado que la DLL publica en memoria compartida para que CamSolid.exe lo
// muestre. La DLL corre dentro del servicio Frame Server, en otra sesión de
// Windows, así que usa el espacio de nombres Global. Si se carga en un proceso
// normal (por ejemplo CamSolidSelfTest) no puede crear objetos globales y usa Local.
inline constexpr wchar_t CAMSOLID_STATUS_GLOBAL_NAME[] = L"Global\\CamSolidStatus";
inline constexpr wchar_t CAMSOLID_STATUS_LOCAL_NAME[] = L"Local\\CamSolidStatus";
inline constexpr uint32_t CAMSOLID_STATUS_VERSION = 1;

// Si el estado no se actualiza en este tiempo, la DLL ya no está en uso
inline constexpr uint64_t CAMSOLID_STATUS_TIMEOUT_MS = 3000;

enum class StreamState : uint32_t
{
    Stopped = 0,          // Ninguna aplicación usa la cámara
    WaitingForPhone = 1,  // Una aplicación la usa, pero no llega video del celular
    Streaming = 2,        // Llega video del celular
};

struct CamSolidStatus
{
    LONG sequence;                 // Impar mientras la DLL escribe
    uint32_t version;
    StreamState state;
    uint32_t width;                // Formato que eligió la aplicación
    uint32_t height;
    uint32_t phoneWidth;           // Tamaño que envía el celular
    uint32_t phoneHeight;
    uint32_t framesPerSecondX100;
    uint32_t latencyMicros;        // 0 mientras no se han sincronizado los relojes
    uint32_t decodeMicros;
    uint32_t startupMicros;        // Desde que se abrió la cámara hasta el primer cuadro del celular
    uint64_t updatedTickMs;        // GetTickCount64 de la última actualización
    wchar_t deviceName[64];
};

// Copia el estado sin que la DLL lo esté escribiendo a la vez
inline bool ReadStatus(const CamSolidStatus* shared, CamSolidStatus& copy)
{
    for (int attempt = 0; attempt < 5; attempt++)
    {
        const LONG before = shared->sequence;
        MemoryBarrier();
        if (before & 1)
        {
            continue;
        }
        memcpy(&copy, shared, sizeof(copy));
        MemoryBarrier();
        if (shared->sequence == before)
        {
            return copy.version == CAMSOLID_STATUS_VERSION;
        }
    }
    return false;
}
