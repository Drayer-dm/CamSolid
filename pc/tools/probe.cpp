// Busca la cámara CamSolid, lee sus cuadros durante 3 segundos e informa del
// resultado. Si se le pasa una ruta, guarda ahí un cuadro como imagen BMP.
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include "CamSolidIds.h"

using Microsoft::WRL::ComPtr;

bool SaveFrameAsBmp(const wchar_t* path, const BYTE* frame, LONG stride, UINT32 width, UINT32 height);

static int ReportError(const wchar_t* step, HRESULT hr)
{
    wprintf(L"Error en %s: 0x%08X\n", step, static_cast<unsigned int>(hr));
    return 1;
}

static HRESULT FindCamera(ComPtr<IMFActivate>& camera)
{
    ComPtr<IMFAttributes> filter;
    HRESULT hr = MFCreateAttributes(&filter, 1);
    if (FAILED(hr))
    {
        return hr;
    }
    hr = filter->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr))
    {
        return hr;
    }

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(filter.Get(), &devices, &count);
    if (FAILED(hr))
    {
        return hr;
    }

    wprintf(L"Camaras encontradas: %u\n", count);
    for (UINT32 i = 0; i < count; i++)
    {
        wchar_t* name = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)))
        {
            wprintf(L"  - %s\n", name);
            // Windows añade " (Windows Virtual Camera)" al nombre
            if (!camera && wcsncmp(name, CAMSOLID_FRIENDLY_NAME, wcslen(CAMSOLID_FRIENDLY_NAME)) == 0)
            {
                camera = devices[i];
            }
            CoTaskMemFree(name);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return S_OK;
}

static int ReadFrames(IMFActivate* camera, const wchar_t* imagePath)
{
    using Clock = std::chrono::steady_clock;

    ComPtr<IMFMediaSource> source;
    HRESULT hr = camera->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr))
    {
        return ReportError(L"ActivateObject", hr);
    }

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
    if (FAILED(hr))
    {
        return ReportError(L"MFCreateSourceReaderFromMediaSource", hr);
    }

    const DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    ComPtr<IMFMediaType> type;
    hr = reader->GetCurrentMediaType(videoStream, &type);
    if (FAILED(hr))
    {
        return ReportError(L"GetCurrentMediaType", hr);
    }

    UINT32 width = 0;
    UINT32 height = 0;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    const LONG stride = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, width));

    GUID subtype{};
    type->GetGUID(MF_MT_SUBTYPE, &subtype);
    const bool isNv12 = subtype == MFVideoFormat_NV12;
    wprintf(L"Formato: %ux%u, %s\n", width, height, isNv12 ? L"NV12" : L"otro");

    int frameCount = 0;
    bool imagePending = imagePath != nullptr && isNv12;
    const auto start = Clock::now();
    const auto end = start + std::chrono::seconds(3);

    while (Clock::now() < end)
    {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(videoStream, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr))
        {
            return ReportError(L"ReadSample", hr);
        }
        if (flags & MF_SOURCE_READERF_ERROR)
        {
            wprintf(L"Error: la camara dejo de entregar video\n");
            return 1;
        }
        if (!sample)
        {
            continue;
        }

        frameCount++;

        // Se guarda un cuadro a los 2,5 s: el celular tarda en encender su cámara
        if (imagePending && Clock::now() - start >= std::chrono::milliseconds(2500))
        {
            ComPtr<IMFMediaBuffer> buffer;
            BYTE* data = nullptr;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, nullptr)))
            {
                if (SaveFrameAsBmp(imagePath, data, stride, width, height))
                {
                    wprintf(L"Imagen guardada en %s\n", imagePath);
                }
                buffer->Unlock();
            }
            imagePending = false;
        }
    }

    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    wprintf(L"Cuadros recibidos: %d en %.1f s (%.1f por segundo)\n", frameCount, seconds, frameCount / seconds);
    return frameCount > 0 ? 0 : 1;
}

static int RunTest(const wchar_t* imagePath)
{
    ComPtr<IMFActivate> camera;
    const HRESULT hr = FindCamera(camera);
    if (FAILED(hr))
    {
        return ReportError(L"FindCamera", hr);
    }
    if (!camera)
    {
        wprintf(L"No se encontro %s. Comprueba que CamSolid.exe este abierto.\n", CAMSOLID_FRIENDLY_NAME);
        return 1;
    }

    const int exitCode = ReadFrames(camera.Get(), imagePath);
    camera->ShutdownObject();
    return exitCode;
}

int wmain(int argc, wchar_t** argv)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        return ReportError(L"CoInitializeEx", hr);
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        CoUninitialize();
        return ReportError(L"MFStartup", hr);
    }

    const int exitCode = RunTest(argc > 1 ? argv[1] : nullptr);

    MFShutdown();
    CoUninitialize();
    return exitCode;
}
