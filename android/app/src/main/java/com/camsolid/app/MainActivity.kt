package com.camsolid.app

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.RectF
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Size
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity(), StreamServer.Callbacks, CameraStreamer.Listener {

    private lateinit var preview: TextureView
    private lateinit var statusText: TextView
    private lateinit var statsText: TextView
    private lateinit var switchButton: Button

    private lateinit var streamer: CameraStreamer
    private val server = StreamServer(this)

    private var cameraIndex = 0
    @Volatile private var connection: StreamServer.Connection? = null
    @Volatile private var currentRequest: StreamRequest? = null

    // Lo que está transmitiendo la cámara, aunque el PC se haya desconectado
    @Volatile private var activeRequest: StreamRequest? = null
    @Volatile private var lastCodecConfig: ByteArray? = null
    @Volatile private var waitingForKeyFrame = true
    private val handler = Handler(Looper.getMainLooper())

    companion object {
        // Tiempo que la cámara sigue encendida después de que el PC se desconecta
        const val IDLE_TIMEOUT_MS = 60_000L
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        streamer = CameraStreamer(this, this)
        streamer.options = readTestOptions()
        setContentView(buildLayout())
    }

    // Opciones para las pruebas de latencia, por ejemplo:
    // adb shell am start -n com.camsolid.app/.MainActivity --ez plantillaVistaPrevia false
    private fun readTestOptions(): CameraStreamer.Options {
        val defaults = CameraStreamer.Options()
        val extras = intent.extras ?: return defaults
        return CameraStreamer.Options(
            previewTemplate = extras.getBoolean("plantillaVistaPrevia", defaults.previewTemplate),
            stabilization = extras.getBoolean("estabilizacion", defaults.stabilization),
            fastNoiseReduction = extras.getBoolean("ruidoRapido", defaults.fastNoiseReduction),
            showPreview = extras.getBoolean("vistaPrevia", defaults.showPreview),
            mtkLowLatency = extras.getBoolean("mtkBajaLatencia", defaults.mtkLowLatency),
            mtkWifiDisplay = extras.getBoolean("mtkWifiDisplay", defaults.mtkWifiDisplay),
            mtkNoPreprocessing = extras.getBoolean("mtkSinPreprocesado", defaults.mtkNoPreprocessing),
        )
    }

    private fun buildLayout(): View {
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }

        preview = TextureView(this)
        root.addView(preview, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER))

        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.argb(150, 0, 0, 0))
            val padding = dp(12)
            setPadding(padding, padding, padding, padding)
        }
        statusText = TextView(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
        }
        statsText = TextView(this).apply {
            setTextColor(Color.rgb(140, 230, 140))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
        }
        switchButton = Button(this).apply {
            text = "Cambiar cámara"
            setOnClickListener { switchCamera() }
        }
        panel.addView(statusText)
        panel.addView(statsText)
        panel.addView(switchButton)
        root.addView(panel, FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START))
        return root
    }

    override fun onResume() {
        super.onResume()
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            server.start()
        } else {
            setStatus("CamSolid necesita permiso para usar la cámara")
            requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
        }
    }

    override fun onPause() {
        super.onPause()
        handler.removeCallbacks(idleStop)
        server.stop()
        streamer.stop()
        activeRequest = null
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) {
            server.start()
        } else {
            setStatus("Sin permiso de cámara CamSolid no puede funcionar")
        }
    }

    private fun switchCamera() {
        val cameras = streamer.cameraIds()
        if (cameras.size < 2) return
        cameraIndex = (cameraIndex + 1) % cameras.size
        val request = currentRequest ?: return
        startStreaming(request)
    }

    private fun startStreaming(request: StreamRequest) {
        val cameras = streamer.cameraIds()
        if (cameras.isEmpty()) {
            setStatus("No se encontró ninguna cámara")
            return
        }
        val cameraId = cameras[cameraIndex % cameras.size]
        activeRequest = request
        lastCodecConfig = null
        waitingForKeyFrame = true
        streamer.start(cameraId, request, preview.surfaceTexture)
    }

    // Apaga la cámara si el PC no vuelve a conectarse a tiempo
    private val idleStop = Runnable {
        streamer.stop()
        activeRequest = null
        setStatus("Esperando al PC… Conecta el celular por USB y abre CamSolid en el PC")
    }

    // StreamServer.Callbacks (se llaman desde el hilo del servidor)

    override fun onWaiting() {
        if (activeRequest == null) {
            setStatus("Esperando al PC… Conecta el celular por USB y abre CamSolid en el PC")
        }
        setStats("")
    }

    override fun onStreamRequested(request: StreamRequest, connection: StreamServer.Connection) {
        currentRequest = request
        runOnUiThread {
            handler.removeCallbacks(idleStop)
            waitingForKeyFrame = true
            if (request == activeRequest) {
                // La cámara sigue encendida: basta con un cuadro clave para que el PC
                // pueda decodificar desde ya. Primero van los parámetros del códec.
                lastCodecConfig?.let { connection.send(Protocol.CODEC_CONFIG, it, 0) }
                this.connection = connection
                streamer.requestKeyFrame()
                setStatus("Transmitiendo ${request.width}x${request.height} a ${request.frameRate} cuadros/s")
            } else {
                this.connection = connection
                setStatus("El PC pide ${request.width}x${request.height} a ${request.frameRate} cuadros/s")
                startStreaming(request)
            }
        }
    }

    override fun onDisconnected() {
        connection = null
        currentRequest = null
        // La cámara sigue encendida un rato: si una aplicación vuelve a abrirla en el
        // PC, el video llega al instante en vez de esperar a que la cámara arranque
        runOnUiThread {
            if (activeRequest != null) {
                setStatus("En espera: la cámara sigue lista ${IDLE_TIMEOUT_MS / 1000} s para reconectar al instante")
                handler.removeCallbacks(idleStop)
                handler.postDelayed(idleStop, IDLE_TIMEOUT_MS)
            }
        }
    }

    override fun onStats(stats: PcStats) {
        val camera = streamer.cameraDelayMicros / 1000.0
        val encoder = streamer.encoderDelayMicros / 1000.0
        val text = if (stats.latencyMs > 0) {
            "Latencia %.0f ms · %.0f cuadros/s\ncámara %.0f ms · codificador %.0f ms · PC %.0f ms".format(
                stats.latencyMs, stats.framesPerSecond, camera, encoder - camera, stats.latencyMs - encoder)
        } else {
            "%.0f cuadros/s · cámara %.0f ms · codificador %.0f ms".format(stats.framesPerSecond, camera, encoder - camera)
        }
        setStats(text)
        // Registro para las pruebas desde el PC
        Log.i(CameraStreamer.TAG, "STATS latencia=%.1f fps=%.1f camara=%.1f salidaCodificador=%.1f decodPC=%.1f esperaPC=%.1f exposicion=%.1f codificador=%s".format(
            stats.latencyMs, stats.framesPerSecond, camera, encoder, stats.decodeMs, stats.waitMs, streamer.exposureMicros / 1000.0, streamer.encoderName))
    }

    override fun clockMicros(): Long = streamer.clockMicros()

    // CameraStreamer.Listener

    override fun onCodecConfig(data: ByteArray) {
        lastCodecConfig = data
        connection?.send(Protocol.CODEC_CONFIG, data, 0)
    }

    override fun onFrame(data: ByteArray, timeMicros: Long, keyFrame: Boolean) {
        val current = connection ?: return
        // Tras conectar, el PC solo puede decodificar a partir de un cuadro clave
        if (waitingForKeyFrame) {
            if (!keyFrame) return
            waitingForKeyFrame = false
        }
        current.send(if (keyFrame) Protocol.KEY_FRAME else Protocol.FRAME, data, timeMicros)
    }

    override fun onStarted(size: Size, frameRate: Int) {
        setStatus("Transmitiendo ${size.width}x${size.height} a $frameRate cuadros/s")
        runOnUiThread { fitPreview(size) }
    }

    override fun onError(message: String) {
        setStatus(message)
    }

    // Ajusta la vista previa a la proporción del video, sin deformarla. La vista
    // gira la imagen de la cámara como si el celular estuviera vertical, así que
    // en horizontal hay que deshacer ese giro.
    private fun fitPreview(size: Size) {
        val parent = preview.parent as View
        val scale = minOf(parent.width.toFloat() / size.width, parent.height.toFloat() / size.height)
        val viewWidth = size.width * scale
        val viewHeight = size.height * scale
        preview.layoutParams = FrameLayout.LayoutParams(viewWidth.toInt(), viewHeight.toInt(), Gravity.CENTER)

        @Suppress("DEPRECATION")
        val rotation = windowManager.defaultDisplay.rotation
        val matrix = Matrix()
        if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
            val viewRect = RectF(0f, 0f, viewWidth, viewHeight)
            val bufferRect = RectF(0f, 0f, size.height.toFloat(), size.width.toFloat())
            val centerX = viewRect.centerX()
            val centerY = viewRect.centerY()
            bufferRect.offset(centerX - bufferRect.centerX(), centerY - bufferRect.centerY())
            matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
            val fill = maxOf(viewHeight / size.height, viewWidth / size.width)
            matrix.postScale(fill, fill, centerX, centerY)
            matrix.postRotate(90f * (rotation - 2), centerX, centerY)
        }
        preview.setTransform(matrix)
    }

    private fun setStatus(text: String) = runOnUiThread { statusText.text = text }

    private fun setStats(text: String) = runOnUiThread { statsText.text = text }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}
