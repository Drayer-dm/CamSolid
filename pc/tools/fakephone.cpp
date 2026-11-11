// Simula la app del celular para probar el PC sin teléfono. Escucha en el puerto
// de CamSolid, recibe la petición de video, genera una imagen en movimiento, la
// comprime en H.264 con Media Foundation y la envía con el mismo protocolo.
// También contesta los Ping y muestra la latencia que le informa el PC.
//
// Uso: CamSolidFakePhone [segundos]   (por defecto, 20)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <codecapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>
#include "Clock.h"
#include "Protocol.h"

using Microsoft::WRL::ComPtr;

#define RETURN_IF_FAILED(expression) \
    do { const HRESULT hr_ = (expression); if (FAILED(hr_)) { wprintf(L"Fallo %S: 0x%08X\n", #expression, static_cast<unsigned int>(hr_)); return hr_; } } while (0)

struct StreamSettings
{
    UINT32 width = 1280;
    UINT32 height = 720;
    UINT32 frameRate = 30;
    UINT32 bitrate = 6'000'000;
};

// Tiempo que pasan los cuadros dentro del codificador, para separarlo de la latencia del PC
static int64_t g_encoderDelaySum = 0;
static int64_t g_encoderDelayCount = 0;

static bool SendAll(SOCKET connection, const BYTE* data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        const int count = send(connection, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
        if (count <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(count);
    }
    return true;
}

static bool ReceiveAll(SOCKET connection, BYTE* data, size_t size)
{
    size_t received = 0;
    while (received < size)
    {
        const int count = recv(connection, reinterpret_cast<char*>(data + received), static_cast<int>(size - received), 0);
        if (count <= 0)
        {
            return false;
        }
        received += static_cast<size_t>(count);
    }
    return true;
}

static bool SendPacket(SOCKET connection, MessageType type, const BYTE* data, uint32_t size, int64_t time)
{
    std::vector<BYTE> packet(CAMSOLID_HEADER_SIZE + size);
    WriteHeader(packet.data(), type, size, time);
    if (size > 0)
    {
        memcpy(packet.data() + CAMSOLID_HEADER_SIZE, data, size);
    }
    return SendAll(connection, packet.data(), packet.size());
}

// Fondo gris con un cuadrado rojo que rebota, distinto de la imagen de prueba de la DLL
static void DrawFrame(std::vector<BYTE>& frame, UINT32 width, UINT32 height, UINT64 frameNumber)
{
    const UINT32 size = (height / 4) & ~1u;
    const UINT32 rangeX = width - size;
    const UINT32 rangeY = height - size;
    UINT32 x = static_cast<UINT32>(frameNumber * 12 % (2 * rangeX));
    UINT32 y = static_cast<UINT32>(frameNumber * 7 % (2 * rangeY));
    x = (x < rangeX ? x : 2 * rangeX - x) & ~1u;
    y = (y < rangeY ? y : 2 * rangeY - y) & ~1u;

    BYTE* planeY = frame.data();
    BYTE* planeUv = planeY + static_cast<size_t>(width) * height;
    memset(planeY, 90, static_cast<size_t>(width) * height);
    memset(planeUv, 128, static_cast<size_t>(width) * height / 2);

    for (UINT32 row = y; row < y + size; row++)
    {
        memset(planeY + static_cast<size_t>(width) * row + x, 81, size);
    }
    for (UINT32 row = y / 2; row < (y + size) / 2; row++)
    {
        BYTE* line = planeUv + static_cast<size_t>(width) * row;
        for (UINT32 column = x; column < x + size; column += 2)
        {
            line[column] = 90;
            line[column + 1] = 240;
        }
    }
}

// Igual que pedirá la app de Android: baja latencia, sin cuadros B y un cuadro
// clave por segundo. Debe hacerse antes de fijar los formatos.
static void ConfigureLowLatency(IMFTransform* encoder, const StreamSettings& settings)
{
    ComPtr<ICodecAPI> codecApi;
    if (FAILED(encoder->QueryInterface(IID_PPV_ARGS(&codecApi))))
    {
        wprintf(L"El codificador no admite ICodecAPI\n");
        return;
    }

    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_TRUE;
    const HRESULT lowLatency = codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &value);

    value.vt = VT_UI4;
    value.ulVal = settings.frameRate;
    codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &value);

    value.ulVal = 0;
    codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);

    value.ulVal = eAVEncCommonRateControlMode_CBR;
    codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &value);

    wprintf(L"Modo de baja latencia: 0x%08X\n", static_cast<unsigned int>(lowLatency));
}

static HRESULT CreateEncoder(const StreamSettings& settings, ComPtr<IMFTransform>& encoder)
{
    MFT_REGISTER_TYPE_INFO output = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    RETURN_IF_FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &output, &activates, &count));
    HRESULT hr = count > 0 ? activates[0]->ActivateObject(IID_PPV_ARGS(&encoder)) : MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; i++)
    {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    RETURN_IF_FAILED(hr);

    ConfigureLowLatency(encoder.Get(), settings);

    ComPtr<IMFMediaType> outputType;
    RETURN_IF_FAILED(MFCreateMediaType(&outputType));
    RETURN_IF_FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    RETURN_IF_FAILED(outputType->SetUINT32(MF_MT_AVG_BITRATE, settings.bitrate));
    RETURN_IF_FAILED(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, settings.width, settings.height));
    RETURN_IF_FAILED(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, settings.frameRate, 1));
    RETURN_IF_FAILED(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RETURN_IF_FAILED(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base));
    RETURN_IF_FAILED(encoder->SetOutputType(0, outputType.Get(), 0));

    ComPtr<IMFMediaType> inputType;
    RETURN_IF_FAILED(MFCreateMediaType(&inputType));
    RETURN_IF_FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    RETURN_IF_FAILED(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, settings.width, settings.height));
    RETURN_IF_FAILED(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, settings.frameRate, 1));
    RETURN_IF_FAILED(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(encoder->SetInputType(0, inputType.Get(), 0));

    RETURN_IF_FAILED(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    RETURN_IF_FAILED(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    return S_OK;
}

// Saca del codificador los cuadros comprimidos y los envía
static HRESULT SendEncodedFrames(IMFTransform* encoder, SOCKET connection, int& framesSent)
{
    for (;;)
    {
        MFT_OUTPUT_STREAM_INFO info{};
        RETURN_IF_FAILED(encoder->GetOutputStreamInfo(0, &info));
        const bool encoderAllocates = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        ComPtr<IMFSample> ownSample;
        if (!encoderAllocates)
        {
            ComPtr<IMFMediaBuffer> buffer;
            RETURN_IF_FAILED(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 1024 * 1024), &buffer));
            RETURN_IF_FAILED(MFCreateSample(&ownSample));
            RETURN_IF_FAILED(ownSample->AddBuffer(buffer.Get()));
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = ownSample.Get();
        DWORD status = 0;
        const HRESULT hr = encoder->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents)
        {
            output.pEvents->Release();
        }
        ComPtr<IMFSample> sample;
        if (encoderAllocates)
        {
            sample.Attach(output.pSample);
        }
        else
        {
            sample = ownSample;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            return S_OK;
        }
        RETURN_IF_FAILED(hr);

        LONGLONG sampleTime = 0;
        sample->GetSampleTime(&sampleTime);
        const bool keyFrame = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;

        ComPtr<IMFMediaBuffer> buffer;
        RETURN_IF_FAILED(sample->ConvertToContiguousBuffer(&buffer));
        BYTE* data = nullptr;
        DWORD length = 0;
        RETURN_IF_FAILED(buffer->Lock(&data, nullptr, &length));
        // El tiempo de la muestra es el de captura, en unidades de 100 ns
        const bool sent = SendPacket(connection, keyFrame ? MessageType::KeyFrame : MessageType::Frame, data, length, sampleTime / 10);
        buffer->Unlock();
        if (!sent)
        {
            return HRESULT_FROM_WIN32(WSAECONNRESET);
        }
        framesSent++;
        g_encoderDelaySum += NowMicros() - sampleTime / 10;
        g_encoderDelayCount++;
    }
}

// Contesta los Ping y muestra las estadísticas, sin bloquear si no hay nada
static bool HandleIncoming(SOCKET connection)
{
    for (;;)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(connection, &readable);
        timeval timeout{ 0, 0 };
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready == 0)
        {
            return true;
        }
        if (ready == SOCKET_ERROR)
        {
            return false;
        }

        BYTE headerData[CAMSOLID_HEADER_SIZE];
        if (!ReceiveAll(connection, headerData, sizeof(headerData)))
        {
            return false;
        }
        const MessageHeader header = ParseHeader(headerData);
        std::vector<BYTE> payload(header.size);
        if (header.size > 0 && !ReceiveAll(connection, payload.data(), payload.size()))
        {
            return false;
        }

        if (header.type == MessageType::Ping)
        {
            BYTE phoneTime[8];
            WriteInt64BE(phoneTime, NowMicros());
            if (!SendPacket(connection, MessageType::Pong, phoneTime, sizeof(phoneTime), header.time))
            {
                return false;
            }
        }
        else if (header.type == MessageType::Stats && payload.size() >= 12)
        {
            const double encoderDelay = g_encoderDelayCount > 0 ? g_encoderDelaySum / 1000.0 / g_encoderDelayCount : 0;
            g_encoderDelaySum = 0;
            g_encoderDelayCount = 0;
            const double wait = payload.size() >= 16 ? ReadUInt32BE(payload.data() + 12) / 1000.0 : 0;
            wprintf(L"  PC: latencia %.1f ms, %.1f cuadros/s, decodificacion %.1f ms, espera %.1f ms | codificador %.1f ms\n",
                ReadUInt32BE(payload.data()) / 1000.0, ReadUInt32BE(payload.data() + 4) / 100.0, ReadUInt32BE(payload.data() + 8) / 1000.0, wait, encoderDelay);
        }
    }
}

static void ServeConnection(SOCKET connection, int64_t deadline)
{
    BYTE headerData[CAMSOLID_HEADER_SIZE];
    if (!ReceiveAll(connection, headerData, sizeof(headerData)))
    {
        return;
    }
    const MessageHeader header = ParseHeader(headerData);
    std::vector<BYTE> payload(header.size);
    if (header.type != MessageType::StreamRequest || header.size < 20 || !ReceiveAll(connection, payload.data(), payload.size()))
    {
        wprintf(L"Peticion de video no valida\n");
        return;
    }

    StreamSettings settings;
    settings.width = ReadUInt32BE(payload.data() + 4);
    settings.height = ReadUInt32BE(payload.data() + 8);
    settings.frameRate = ReadUInt32BE(payload.data() + 12);
    settings.bitrate = ReadUInt32BE(payload.data() + 16);
    wprintf(L"El PC pide %ux%u a %u cuadros/s, %.1f Mbit/s\n", settings.width, settings.height, settings.frameRate, settings.bitrate / 1e6);

    const char name[] = "Simulador de celular";
    SendPacket(connection, MessageType::DeviceInfo, reinterpret_cast<const BYTE*>(name), sizeof(name) - 1, 0);

    ComPtr<IMFTransform> encoder;
    if (FAILED(CreateEncoder(settings, encoder)))
    {
        return;
    }

    std::vector<BYTE> frame(static_cast<size_t>(settings.width) * settings.height * 3 / 2);
    const int64_t period = 1'000'000 / settings.frameRate;
    int64_t nextFrame = NowMicros();
    UINT64 frameNumber = 0;
    int framesSent = 0;

    while (NowMicros() < deadline)
    {
        // Mientras llega la hora del siguiente cuadro, contesta los Ping al instante
        bool connected = true;
        for (int64_t wait = nextFrame - NowMicros(); connected && wait > 0; wait = nextFrame - NowMicros())
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(connection, &readable);
            timeval timeout{ 0, static_cast<long>(wait) };
            if (select(0, &readable, nullptr, nullptr, &timeout) > 0)
            {
                connected = HandleIncoming(connection);
            }
        }
        nextFrame += period;

        if (!connected || !HandleIncoming(connection))
        {
            break;
        }

        DrawFrame(frame, settings.width, settings.height, frameNumber++);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), &buffer)))
        {
            break;
        }
        BYTE* data = nullptr;
        buffer->Lock(&data, nullptr, nullptr);
        memcpy(data, frame.data(), frame.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(frame.size()));

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        // Se usa el momento de "captura" como tiempo de la muestra
        sample->SetSampleTime(NowMicros() * 10);
        sample->SetSampleDuration(period * 10);

        if (FAILED(encoder->ProcessInput(0, sample.Get(), 0)) || FAILED(SendEncodedFrames(encoder.Get(), connection, framesSent)))
        {
            break;
        }
    }
    wprintf(L"Conexion terminada: %d cuadros enviados\n", framesSent);
}

int wmain(int argc, wchar_t** argv)
{
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 20;

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(CAMSOLID_PORT);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0)
    {
        wprintf(L"No se pudo escuchar en el puerto %u (error %d). Quiza adb forward lo esta usando.\n", CAMSOLID_PORT, WSAGetLastError());
        return 1;
    }
    wprintf(L"Simulador escuchando en 127.0.0.1:%u durante %d s\n", CAMSOLID_PORT, seconds);

    const int64_t deadline = NowMicros() + static_cast<int64_t>(seconds) * 1'000'000;
    while (NowMicros() < deadline)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{ 0, 200'000 };
        if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
        {
            continue;
        }

        SOCKET connection = accept(listener, nullptr, nullptr);
        if (connection == INVALID_SOCKET)
        {
            continue;
        }
        const BOOL noDelay = TRUE;
        setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        wprintf(L"PC conectado\n");
        ServeConnection(connection, deadline);
        closesocket(connection);
    }

    closesocket(listener);
    MFShutdown();
    CoUninitialize();
    WSACleanup();
    return 0;
}
