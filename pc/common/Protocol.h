#pragma once

#include <cstdint>

// Protocolo entre el celular y el PC. Lo implementan la DLL (PhoneReceiver), el
// simulador de celular (tools/fakephone.cpp) y la app de Android (Protocol.kt).
//
// Conexión: el celular escucha en el puerto TCP CAMSOLID_PORT y "adb forward"
// redirige ese mismo puerto del PC hacia él. Se conecta el PC.
//
// Cada mensaje, en las dos direcciones, es una cabecera de 16 bytes seguida de
// "size" bytes de datos. Todos los números van en big-endian (orden de red),
// que es como los escribe Java.
//
//   uint32 type    Uno de los MessageType de abajo
//   uint32 size    Bytes de datos que siguen a la cabecera
//   int64  time    Microsegundos; su significado depende del tipo

inline constexpr uint16_t CAMSOLID_PORT = 27183;
inline constexpr uint32_t CAMSOLID_PROTOCOL_VERSION = 1;
inline constexpr uint32_t CAMSOLID_HEADER_SIZE = 16;
inline constexpr uint32_t CAMSOLID_MAX_MESSAGE_SIZE = 8 * 1024 * 1024;

enum class MessageType : uint32_t
{
    // PC -> celular, al conectar. Datos: version, width, height, frameRate, bitrate (uint32 cada uno)
    StreamRequest = 1,
    // PC -> celular. time = reloj del PC al enviar. Sin datos.
    Ping = 2,
    // PC -> celular, cada segundo. Datos (uint32 cada uno): latencyMicros, framesPerSecond x100,
    // decodeMicros y waitMicros (desde que se decodifica hasta que la aplicación recibe el cuadro)
    Stats = 3,

    // Celular -> PC. Parámetros del códec (SPS y PPS en formato Annex B). time = 0.
    CodecConfig = 101,
    // Celular -> PC. Un cuadro H.264 en formato Annex B. time = captura, en el reloj del celular.
    Frame = 102,
    // Igual que Frame, pero es un cuadro clave que se puede decodificar por sí solo.
    KeyFrame = 103,
    // Celular -> PC. time = el del Ping recibido. Datos: reloj del celular (int64, microsegundos).
    Pong = 104,
    // Celular -> PC. Texto UTF-8 con el nombre del dispositivo, para mostrarlo.
    DeviceInfo = 105,
};

struct MessageHeader
{
    MessageType type;
    uint32_t size;
    int64_t time;
};

inline uint32_t ReadUInt32BE(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

inline int64_t ReadInt64BE(const uint8_t* data)
{
    return static_cast<int64_t>((static_cast<uint64_t>(ReadUInt32BE(data)) << 32) | ReadUInt32BE(data + 4));
}

inline void WriteUInt32BE(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

inline void WriteInt64BE(uint8_t* data, int64_t value)
{
    WriteUInt32BE(data, static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32));
    WriteUInt32BE(data + 4, static_cast<uint32_t>(value));
}

inline MessageHeader ParseHeader(const uint8_t* data)
{
    return { static_cast<MessageType>(ReadUInt32BE(data)), ReadUInt32BE(data + 4), ReadInt64BE(data + 8) };
}

inline void WriteHeader(uint8_t* data, MessageType type, uint32_t size, int64_t time)
{
    WriteUInt32BE(data, static_cast<uint32_t>(type));
    WriteUInt32BE(data + 4, size);
    WriteInt64BE(data + 8, time);
}
