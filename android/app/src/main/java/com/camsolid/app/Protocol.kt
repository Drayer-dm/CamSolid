package com.camsolid.app

// Protocolo entre el celular y el PC. Es el mismo de pc/common/Protocol.h:
// cada mensaje es una cabecera de 16 bytes (type, size, time) seguida de
// "size" bytes de datos, todo en big-endian.
object Protocol {
    const val PORT = 27183
    const val VERSION = 1
    const val HEADER_SIZE = 16
    const val MAX_MESSAGE_SIZE = 8 * 1024 * 1024

    // PC -> celular
    const val STREAM_REQUEST = 1
    const val PING = 2
    const val STATS = 3

    // Celular -> PC
    const val CODEC_CONFIG = 101
    const val FRAME = 102
    const val KEY_FRAME = 103
    const val PONG = 104
    const val DEVICE_INFO = 105
}

// Lo que pide el PC al conectarse
data class StreamRequest(val width: Int, val height: Int, val frameRate: Int, val bitrate: Int)

// Lo que informa el PC cada segundo
data class PcStats(val latencyMs: Double, val framesPerSecond: Double, val decodeMs: Double, val waitMs: Double)
