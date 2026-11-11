#pragma once

#include <windows.h>
#include <cstdint>

// Reloj monotónico del PC en microsegundos. Es el mismo para todos los procesos
// del equipo, así que la DLL y el simulador de celular pueden compararlo.
inline int64_t NowMicros()
{
    static const LONGLONG frequency = []
    {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart / frequency * 1'000'000 + counter.QuadPart % frequency * 1'000'000 / frequency;
}
