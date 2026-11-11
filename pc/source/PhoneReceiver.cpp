#include "PhoneReceiver.h"

#include <ws2tcpip.h>
#include <algorithm>
#include <climits>
#include "Clock.h"

namespace
{
    constexpr int64_t RECONNECT_DELAY_MS = 500;
    constexpr int64_t PING_INTERVAL = 1'000'000;
    constexpr int64_t STATS_INTERVAL = 1'000'000;
    constexpr int64_t RECEIVING_TIMEOUT = 1'000'000;
    constexpr int SELECT_TIMEOUT_MS = 100;
}

PhoneReceiver::PhoneReceiver(std::function<void()> onFrame)
    : _onFrame(std::move(onFrame))
{
}

PhoneReceiver::~PhoneReceiver()
{
    Stop();
}

void PhoneReceiver::Start(UINT32 width, UINT32 height, UINT32 frameRate)
{
    {
        std::lock_guard lock(_mutex);
        // Si ya recibe con el mismo formato, se mantiene la conexión
        if (_thread.joinable() && width == _width && height == _height && frameRate == _frameRate)
        {
            return;
        }
    }

    Stop();

    _status.Update([&](CamSolidStatus& status)
    {
        // Se limpia todo menos la secuencia y la versión, que controla StatusPublisher
        const LONG sequence = status.sequence;
        const uint32_t version = status.version;
        status = {};
        status.sequence = sequence;
        status.version = version;
        status.state = StreamState::WaitingForPhone;
        status.width = width;
        status.height = height;
    });

    std::lock_guard lock(_mutex);
    _width = width;
    _height = height;
    _frameRate = frameRate;
    _stopRequested = false;
    _startTime = NowMicros();
    _firstFrameReported = false;
    _thread = std::thread(&PhoneReceiver::Run, this);
}

void PhoneReceiver::Stop()
{
    {
        std::lock_guard lock(_mutex);
        _stopRequested = true;
    }
    _wake.notify_all();
    if (_thread.joinable())
    {
        _thread.join();
    }

    _status.Update([](CamSolidStatus& status) { status.state = StreamState::Stopped; });

    std::lock_guard lock(_frameMutex);
    _latest = {};
    _lastFrameArrival = 0;
}

bool PhoneReceiver::ShouldStop()
{
    std::lock_guard lock(_mutex);
    return _stopRequested;
}

bool PhoneReceiver::IsReceiving()
{
    std::lock_guard lock(_frameMutex);
    return _lastFrameArrival != 0 && NowMicros() - _lastFrameArrival < RECEIVING_TIMEOUT;
}

bool PhoneReceiver::CopyLatestFrame(BYTE* destination, LONG pitch, UINT32 width, UINT32 height, int64_t& captureTime, int64_t& decodedTime)
{
    std::lock_guard lock(_frameMutex);
    if (_latest.data.empty())
    {
        return false;
    }
    CopyNv12(_latest, destination, pitch, width, height);
    captureTime = _latestCaptureTime;
    decodedTime = _latestDecodedTime;
    return true;
}

void PhoneReceiver::ReportDelivered(int64_t captureTime, int64_t decodedTime)
{
    const int64_t now = NowMicros();
    std::lock_guard lock(_frameMutex);
    _latencySum += now - captureTime;
    _waitSum += now - decodedTime;
    _deliveredCount++;
}

void PhoneReceiver::Run()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        return;
    }
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (!ShouldStop())
    {
        SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connection != INVALID_SOCKET)
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(CAMSOLID_PORT);
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

            if (connect(connection, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
            {
                // Envía cada paquete al momento, sin agruparlos
                const BOOL noDelay = TRUE;
                setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                RunConnection(connection);
            }
            closesocket(connection);
        }

        // Sin celular: se publica cada intento para que CamSolid.exe vea que la cámara sigue en uso
        _status.Update([](CamSolidStatus& status)
        {
            status.state = StreamState::WaitingForPhone;
            status.phoneWidth = 0;
            status.phoneHeight = 0;
            status.framesPerSecondX100 = 0;
            status.latencyMicros = 0;
            status.deviceName[0] = L'\0';
        });

        std::unique_lock lock(_mutex);
        _wake.wait_for(lock, std::chrono::milliseconds(RECONNECT_DELAY_MS), [this] { return _stopRequested; });
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    WSACleanup();
}

void PhoneReceiver::RunConnection(SOCKET connection)
{
    H264Decoder decoder;
    if (FAILED(decoder.Initialize()))
    {
        return;
    }

    UINT32 width, height, frameRate;
    {
        std::lock_guard lock(_mutex);
        width = _width;
        height = _height;
        frameRate = _frameRate;
    }

    // Unos 0,25 bits por píxel: 6,9 Mbit/s en 720p a 30 cuadros por segundo
    const UINT32 bitrate = static_cast<UINT32>(static_cast<UINT64>(width) * height * frameRate / 4);
    BYTE request[20];
    WriteUInt32BE(request, CAMSOLID_PROTOCOL_VERSION);
    WriteUInt32BE(request + 4, width);
    WriteUInt32BE(request + 8, height);
    WriteUInt32BE(request + 12, frameRate);
    WriteUInt32BE(request + 16, bitrate);
    if (!SendPacket(connection, MessageType::StreamRequest, request, sizeof(request), NowMicros()))
    {
        return;
    }

    _codecConfig.clear();
    _hasClockOffset = false;
    _bestRoundTrip = LLONG_MAX;
    _lastPingTime = 0;
    _lastStatsTime = NowMicros();

    BYTE headerData[CAMSOLID_HEADER_SIZE];
    for (;;)
    {
        if (!DoPeriodicWork(connection) || !ReceiveExact(connection, headerData, sizeof(headerData)))
        {
            return;
        }

        const MessageHeader header = ParseHeader(headerData);
        if (header.size > CAMSOLID_MAX_MESSAGE_SIZE)
        {
            return;
        }
        _message.resize(header.size);
        if (header.size > 0 && !ReceiveExact(connection, _message.data(), header.size))
        {
            return;
        }

        switch (header.type)
        {
        case MessageType::CodecConfig:
            _codecConfig = _message;
            break;
        case MessageType::Frame:
        case MessageType::KeyFrame:
            HandleFrame(decoder, header);
            break;
        case MessageType::Pong:
            HandlePong(header);
            break;
        case MessageType::DeviceInfo:
            _status.Update([this](CamSolidStatus& status)
            {
                const int length = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(_message.data()), static_cast<int>(_message.size()),
                    status.deviceName, ARRAYSIZE(status.deviceName) - 1);
                status.deviceName[std::max(length, 0)] = L'\0';
            });
            break;
        default:
            break;
        }
    }
}

// Lee exactamente "size" bytes. Mientras espera, sigue enviando los mensajes
// periódicos y comprueba si se pidió detener el receptor.
bool PhoneReceiver::ReceiveExact(SOCKET connection, BYTE* data, size_t size)
{
    size_t received = 0;
    while (received < size)
    {
        // Se comprueba siempre: si el video llega sin pausa, select nunca agota su espera
        if (ShouldStop())
        {
            return false;
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(connection, &readable);
        timeval timeout{ 0, SELECT_TIMEOUT_MS * 1000 };

        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR)
        {
            return false;
        }
        if (ready == 0)
        {
            if (!DoPeriodicWork(connection))
            {
                return false;
            }
            continue;
        }

        const int chunk = static_cast<int>(std::min<size_t>(size - received, INT_MAX));
        const int count = recv(connection, reinterpret_cast<char*>(data + received), chunk, 0);
        if (count <= 0)
        {
            return false;
        }
        received += static_cast<size_t>(count);
    }
    return true;
}

bool PhoneReceiver::SendPacket(SOCKET connection, MessageType type, const BYTE* data, uint32_t size, int64_t time)
{
    std::vector<BYTE> packet(CAMSOLID_HEADER_SIZE + size);
    WriteHeader(packet.data(), type, size, time);
    if (size > 0)
    {
        memcpy(packet.data() + CAMSOLID_HEADER_SIZE, data, size);
    }

    size_t sent = 0;
    while (sent < packet.size())
    {
        const int count = send(connection, reinterpret_cast<const char*>(packet.data() + sent), static_cast<int>(packet.size() - sent), 0);
        if (count <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(count);
    }
    return true;
}

// Mide el reloj del celular (Ping) y le informa de la latencia (Stats)
bool PhoneReceiver::DoPeriodicWork(SOCKET connection)
{
    const int64_t now = NowMicros();

    if (now - _lastPingTime >= PING_INTERVAL)
    {
        _lastPingTime = now;
        if (!SendPacket(connection, MessageType::Ping, nullptr, 0, now))
        {
            return false;
        }
    }

    if (now - _lastStatsTime >= STATS_INTERVAL)
    {
        BYTE stats[16];
        uint32_t latency = 0;
        uint32_t framesPerSecond = 0;
        uint32_t decode = 0;
        uint32_t phoneWidth = 0;
        uint32_t phoneHeight = 0;
        bool receiving = false;
        {
            std::lock_guard lock(_frameMutex);
            const int64_t elapsed = now - _lastStatsTime;
            latency = _hasClockOffset && _deliveredCount > 0 ? static_cast<uint32_t>(_latencySum / _deliveredCount) : 0;
            framesPerSecond = static_cast<uint32_t>(static_cast<int64_t>(_deliveredCount) * 100'000'000 / elapsed);
            decode = _decodedCount > 0 ? static_cast<uint32_t>(_decodeSum / _decodedCount) : 0;
            phoneWidth = _latest.width;
            phoneHeight = _latest.height;
            receiving = _lastFrameArrival != 0 && now - _lastFrameArrival < RECEIVING_TIMEOUT;
            WriteUInt32BE(stats, latency);
            WriteUInt32BE(stats + 4, framesPerSecond);
            WriteUInt32BE(stats + 8, decode);
            WriteUInt32BE(stats + 12, _deliveredCount > 0 ? static_cast<uint32_t>(_waitSum / _deliveredCount) : 0);
            _waitSum = 0;
            _latencySum = 0;
            _deliveredCount = 0;
            _decodeSum = 0;
            _decodedCount = 0;
        }
        _lastStatsTime = now;

        _status.Update([&](CamSolidStatus& status)
        {
            status.state = receiving ? StreamState::Streaming : StreamState::WaitingForPhone;
            status.phoneWidth = phoneWidth;
            status.phoneHeight = phoneHeight;
            status.framesPerSecondX100 = framesPerSecond;
            status.latencyMicros = latency;
            status.decodeMicros = decode;
        });

        if (!SendPacket(connection, MessageType::Stats, stats, sizeof(stats), now))
        {
            return false;
        }
    }
    return true;
}

// El celular contesta cada Ping con su reloj. Se usa la respuesta más rápida,
// porque es la que menos error tiene al suponer que el viaje es simétrico.
void PhoneReceiver::HandlePong(const MessageHeader& header)
{
    if (_message.size() < 8)
    {
        return;
    }
    const int64_t now = NowMicros();
    const int64_t roundTrip = now - header.time;
    const int64_t phoneTime = ReadInt64BE(_message.data());

    if (!_hasClockOffset || roundTrip <= _bestRoundTrip + 1'000)
    {
        _bestRoundTrip = std::min(_bestRoundTrip, roundTrip);
        _clockOffset = phoneTime - (header.time + roundTrip / 2);
        _hasClockOffset = true;
    }
}

void PhoneReceiver::HandleFrame(H264Decoder& decoder, const MessageHeader& header)
{
    const int64_t start = NowMicros();

    // Los parámetros del códec van delante del primer cuadro que los necesita
    const BYTE* data = _message.data();
    DWORD size = static_cast<DWORD>(_message.size());
    if (!_codecConfig.empty())
    {
        _frameData.assign(_codecConfig.begin(), _codecConfig.end());
        _frameData.insert(_frameData.end(), _message.begin(), _message.end());
        _codecConfig.clear();
        data = _frameData.data();
        size = static_cast<DWORD>(_frameData.size());
    }

    if (decoder.Decode(data, size, _decoded) != S_OK)
    {
        return;
    }

    const int64_t end = NowMicros();
    {
        std::lock_guard lock(_frameMutex);
        std::swap(_latest, _decoded);
        _latestCaptureTime = _hasClockOffset ? header.time - _clockOffset : start;
        _lastFrameArrival = end;
        _latestDecodedTime = end;
        _decodeSum += end - start;
        _decodedCount++;
    }

    // Cuánto tardó en llegar video desde que una aplicación abrió la cámara
    if (!_firstFrameReported)
    {
        _firstFrameReported = true;
        const uint32_t startup = static_cast<uint32_t>(end - _startTime);
        _status.Update([startup](CamSolidStatus& status)
        {
            status.state = StreamState::Streaming;
            status.startupMicros = startup;
        });
    }
    _onFrame();
}
