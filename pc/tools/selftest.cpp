// Herramienta de desarrollo: carga CamSolidSource.dll directamente, sin
// registrarla ni pasar por Frame Server, y lee su video con un lector normal.
// Sirve para probar cambios de la DLL sin reiniciar el servicio de cámaras.
//
// Uso: CamSolidSelfTest <ruta de la dll> [imagen.bmp] [segundos] [formato]
// "formato" es el número del formato de la cámara (0 = 1920x1080 a 30, 1 = 1280x720 a 30)
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include "CamSolidIds.h"
#include "Status.h"

using Microsoft::WRL::ComPtr;

using DllGetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using DllCanUnloadNowFunction = HRESULT(STDAPICALLTYPE*)();

bool SaveFrameAsBmp(const wchar_t* path, const BYTE* frame, LONG stride, UINT32 width, UINT32 height);

#define CHECK(expression) \
    do { const HRESULT hr_ = (expression); if (FAILED(hr_)) { wprintf(L"Fallo %S: 0x%08X\n", #expression, static_cast<unsigned int>(hr_)); return 1; } } while (0)

// La DLL cargada en este proceso publica su estado con nombre Local
static void PrintStartupTime()
{
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, CAMSOLID_STATUS_LOCAL_NAME);
    if (!mapping)
    {
        return;
    }
    const auto* shared = static_cast<const CamSolidStatus*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(CamSolidStatus)));
    CamSolidStatus status{};
    if (shared && ReadStatus(shared, status) && status.startupMicros > 0)
    {
        wprintf(L"Primer cuadro del celular: %.0f ms despues de abrir la camara\n", status.startupMicros / 1000.0);
    }
    else
    {
        wprintf(L"No llego ningun cuadro del celular\n");
    }
    if (shared)
    {
        UnmapViewOfFile(shared);
    }
    CloseHandle(mapping);
}

static int ReadVideo(IMFMediaSource* source, const wchar_t* imagePath, int seconds, int formatIndex)
{
    using Clock = std::chrono::steady_clock;
    const DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    ComPtr<IMFSourceReader> reader;
    CHECK(MFCreateSourceReaderFromMediaSource(source, nullptr, &reader));

    ComPtr<IMFMediaType> type;
    if (formatIndex >= 0)
    {
        CHECK(reader->GetNativeMediaType(videoStream, static_cast<DWORD>(formatIndex), &type));
        CHECK(reader->SetCurrentMediaType(videoStream, nullptr, type.Get()));
    }
    CHECK(reader->GetCurrentMediaType(videoStream, &type));
    UINT32 width = 0;
    UINT32 height = 0;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    const LONG stride = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, width));

    int frames = 0;
    double maxGapMs = 0;
    auto lastFrame = Clock::now();
    const auto start = Clock::now();
    while (Clock::now() - start < std::chrono::seconds(seconds))
    {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        CHECK(reader->ReadSample(videoStream, 0, nullptr, &flags, nullptr, &sample));
        if (!sample)
        {
            continue;
        }
        const auto now = Clock::now();
        if (frames > 0)
        {
            maxGapMs = std::max(maxGapMs, std::chrono::duration<double, std::milli>(now - lastFrame).count());
        }
        lastFrame = now;
        frames++;

        // Se guarda un cuadro a mitad de la prueba, cuando el celular ya debería estar enviando
        if (imagePath && frames == seconds * 15)
        {
            ComPtr<IMFMediaBuffer> buffer;
            BYTE* data = nullptr;
            CHECK(sample->ConvertToContiguousBuffer(&buffer));
            CHECK(buffer->Lock(&data, nullptr, nullptr));
            SaveFrameAsBmp(imagePath, data, stride, width, height);
            buffer->Unlock();
        }
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    wprintf(L"%ux%u: %d cuadros en %.1f s (%.1f por segundo), mayor pausa %.0f ms\n", width, height, frames, elapsed, frames / elapsed, maxGapMs);
    PrintStartupTime();
    return frames > 0 ? 0 : 1;
}

static int RunTest(HMODULE dll, const wchar_t* imagePath, int seconds, int formatIndex)
{
    const auto getClassObject = reinterpret_cast<DllGetClassObjectFunction>(GetProcAddress(dll, "DllGetClassObject"));
    const auto canUnloadNow = reinterpret_cast<DllCanUnloadNowFunction>(GetProcAddress(dll, "DllCanUnloadNow"));
    if (!getClassObject || !canUnloadNow)
    {
        wprintf(L"Faltan funciones exportadas\n");
        return 1;
    }

    int result = 0;
    {
        ComPtr<IClassFactory> factory;
        CHECK(getClassObject(CAM_SOLID_ID, IID_PPV_ARGS(&factory)));
        ComPtr<IMFActivate> activate;
        CHECK(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate)));
        ComPtr<IMFMediaSource> source;
        CHECK(activate->ActivateObject(IID_PPV_ARGS(&source)));
        result = ReadVideo(source.Get(), imagePath, seconds, formatIndex);
        source.Reset();
        CHECK(activate->ShutdownObject());
    }

    const HRESULT unload = canUnloadNow();
    wprintf(L"Objetos vivos al terminar: %s\n", unload == S_OK ? L"ninguno" : L"quedan objetos");
    return result != 0 ? result : (unload == S_OK ? 0 : 1);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        wprintf(L"Uso: CamSolidSelfTest <dll> [imagen.bmp] [segundos] [formato]\n");
        return 1;
    }
    const wchar_t* imagePath = argc > 2 ? argv[2] : nullptr;
    const int seconds = argc > 3 ? _wtoi(argv[3]) : 4;
    const int formatIndex = argc > 4 ? _wtoi(argv[4]) : -1;

    CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CHECK(MFStartup(MF_VERSION));

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll)
    {
        wprintf(L"No se pudo cargar la DLL: %lu\n", GetLastError());
        return 1;
    }
    const int exitCode = RunTest(dll, imagePath, seconds, formatIndex);

    MFShutdown();
    CoUninitialize();
    wprintf(L"Resultado: %s\n", exitCode == 0 ? L"OK" : L"FALLO");
    return exitCode;
}
