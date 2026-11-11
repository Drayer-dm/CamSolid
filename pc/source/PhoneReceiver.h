#pragma once

#include <winsock2.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "H264Decoder.h"
#include "Nv12.h"
#include "Protocol.h"
#include "StatusPublisher.h"

// Recibe el video del celular. Un hilo propio se conecta al puerto que "adb
// forward" redirige al teléfono, decodifica cada cuadro H.264 y guarda solo el
// último, para que la cámara nunca entregue imágenes atrasadas.
class PhoneReceiver
{
public:
    // "onFrame" se llama desde el hilo del receptor cada vez que hay un cuadro nuevo
    explicit PhoneReceiver(std::function<void()> onFrame);
    ~PhoneReceiver();

    PhoneReceiver(const PhoneReceiver&) = delete;
    PhoneReceiver& operator=(const PhoneReceiver&) = delete;

    void Start(UINT32 width, UINT32 height, UINT32 frameRate);
    void Stop();

    // true si llegó algún cuadro del celular en el último segundo
    bool IsReceiving();

    // Copia el último cuadro al búfer de la cámara. "captureTime" es el momento
    // en que el celular lo capturó, ya convertido al reloj del PC.
    bool CopyLatestFrame(BYTE* destination, LONG pitch, UINT32 width, UINT32 height, int64_t& captureTime, int64_t& decodedTime);

    // Avisa de que un cuadro llegó a la aplicación, para calcular la latencia
    void ReportDelivered(int64_t captureTime, int64_t decodedTime);

private:
    void Run();
    void RunConnection(SOCKET socket);
    bool ReceiveExact(SOCKET socket, BYTE* data, size_t size);
    bool SendPacket(SOCKET socket, MessageType type, const BYTE* data, uint32_t size, int64_t time);
    bool DoPeriodicWork(SOCKET socket);
    void HandlePong(const MessageHeader& header);
    void HandleFrame(H264Decoder& decoder, const MessageHeader& header);
    bool ShouldStop();

    std::function<void()> _onFrame;
    StatusPublisher _status;

    // Control del hilo
    std::mutex _mutex;
    std::condition_variable _wake;
    std::thread _thread;
    bool _stopRequested = false;
    UINT32 _width = 1280;
    UINT32 _height = 720;
    UINT32 _frameRate = 30;

    // Solo los usa el hilo del receptor
    std::vector<BYTE> _message;
    std::vector<BYTE> _codecConfig;
    std::vector<BYTE> _frameData;
    Nv12Frame _decoded;
    bool _hasClockOffset = false;
    int64_t _clockOffset = 0;   // reloj del celular menos reloj del PC
    int64_t _bestRoundTrip = 0;
    int64_t _lastPingTime = 0;
    int64_t _lastStatsTime = 0;
    int64_t _startTime = 0;
    bool _firstFrameReported = false;

    // Compartido con la cámara, protegido por _frameMutex
    std::mutex _frameMutex;
    Nv12Frame _latest;
    int64_t _latestCaptureTime = 0;
    int64_t _latestDecodedTime = 0;
    int64_t _waitSum = 0;
    int64_t _lastFrameArrival = 0;
    int64_t _latencySum = 0;
    uint32_t _deliveredCount = 0;
    int64_t _decodeSum = 0;
    uint32_t _decodedCount = 0;
};
