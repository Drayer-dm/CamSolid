package com.camsolid.app

import android.os.Build
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket

// Espera la conexión del PC en el puerto de CamSolid. Por USB, "adb forward"
// lleva hasta aquí las conexiones al mismo puerto del PC. Solo escucha en la
// dirección local del celular, así que no es accesible desde la red.
class StreamServer(private val callbacks: Callbacks) {

    interface Callbacks {
        fun onWaiting()
        fun onStreamRequested(request: StreamRequest, connection: Connection)
        fun onDisconnected()
        fun onStats(stats: PcStats)
        fun clockMicros(): Long
    }

    // Una conexión con el PC. Varios hilos envían por ella, así que se sincroniza.
    class Connection(private val socket: Socket) {
        private val output = DataOutputStream(BufferedOutputStream(socket.getOutputStream(), 256 * 1024))

        @Synchronized
        fun send(type: Int, data: ByteArray?, time: Long) {
            try {
                output.writeInt(type)
                output.writeInt(data?.size ?: 0)
                output.writeLong(time)
                if (data != null) output.write(data)
                output.flush()
            } catch (error: IOException) {
                close()
            }
        }

        fun close() {
            try { socket.close() } catch (_: IOException) {}
        }
    }

    @Volatile private var running = false
    private var serverSocket: ServerSocket? = null
    private var connection: Connection? = null
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({ acceptLoop() }, "CamSolidServer").apply { start() }
    }

    fun stop() {
        running = false
        try { serverSocket?.close() } catch (_: IOException) {}
        connection?.close()
        thread?.join(1000)
        thread = null
    }

    private fun acceptLoop() {
        while (running) {
            try {
                ServerSocket(Protocol.PORT, 1, InetAddress.getLoopbackAddress()).use { server ->
                    server.reuseAddress = true
                    serverSocket = server
                    while (running) {
                        callbacks.onWaiting()
                        val socket = server.accept()
                        serve(socket)
                    }
                }
            } catch (error: IOException) {
                if (running) Thread.sleep(500)
            }
        }
    }

    private fun serve(socket: Socket) {
        socket.tcpNoDelay = true
        val current = Connection(socket)
        connection = current
        try {
            val input = DataInputStream(socket.getInputStream().buffered(64 * 1024))
            while (running) {
                val type = input.readInt()
                val size = input.readInt()
                val time = input.readLong()
                if (size < 0 || size > Protocol.MAX_MESSAGE_SIZE) break
                val data = ByteArray(size)
                input.readFully(data)

                when (type) {
                    Protocol.STREAM_REQUEST -> if (size >= 20) {
                        val request = DataInputStream(data.inputStream()).run {
                            readInt() // versión del protocolo
                            StreamRequest(readInt(), readInt(), readInt(), readInt())
                        }
                        current.send(Protocol.DEVICE_INFO, "${Build.MANUFACTURER} ${Build.MODEL}".toByteArray(), 0)
                        callbacks.onStreamRequested(request, current)
                    }
                    // Se contesta al instante: la demora falsearía la sincronización de relojes
                    Protocol.PING -> {
                        val clock = callbacks.clockMicros()
                        val payload = java.nio.ByteBuffer.allocate(8).putLong(clock).array()
                        current.send(Protocol.PONG, payload, time)
                    }
                    Protocol.STATS -> if (size >= 12) {
                        val stats = DataInputStream(data.inputStream()).run {
                            val latency = readInt() / 1000.0
                            val fps = readInt() / 100.0
                            val decode = readInt() / 1000.0
                            val wait = if (size >= 16) readInt() / 1000.0 else 0.0
                            PcStats(latency, fps, decode, wait)
                        }
                        callbacks.onStats(stats)
                    }
                }
            }
        } catch (_: IOException) {
            // El PC se desconectó
        } finally {
            current.close()
            connection = null
            callbacks.onDisconnected()
        }
    }
}
