package com.camsolid.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import kotlin.math.abs

// Captura la cámara y la comprime en H.264. Camera2 dibuja directamente en la
// superficie de entrada del codificador por hardware, sin copias intermedias.
class CameraStreamer(private val context: Context, private val listener: Listener) {

    companion object {
        const val TAG = "CamSolid"
    }

    interface Listener {
        fun onCodecConfig(data: ByteArray)
        fun onFrame(data: ByteArray, timeMicros: Long, keyFrame: Boolean)
        fun onStarted(size: Size, frameRate: Int)
        fun onError(message: String)
    }

    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    private var cameraThread: HandlerThread? = null
    private var cameraHandler: Handler? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    private var encoderSurface: Surface? = null
    private var previewSurface: Surface? = null
    private var outputThread: Thread? = null
    @Volatile private var running = false

    // true si las marcas de tiempo de la cámara usan el reloj elapsedRealtime
    @Volatile var usesRealtimeClock = false
        private set
    @Volatile private var clockDetected = false

    // Tiempo medio que pasa cada cuadro entre la captura y la salida del codificador
    @Volatile var encoderDelayMicros = 0L
        private set

    // Tiempo medio entre la captura y el momento en que la cámara termina el cuadro
    @Volatile var cameraDelayMicros = 0L
        private set

    // Tiempo de exposición del último cuadro: forma parte de la latencia de la cámara
    @Volatile var exposureMicros = 0L
        private set

    // Nombre del codificador en uso, para mostrarlo en las pruebas
    @Volatile var encoderName = ""
        private set

    // El reloj en el que vienen las marcas de tiempo de los cuadros, en microsegundos
    fun clockMicros(): Long =
        if (usesRealtimeClock) SystemClock.elapsedRealtimeNanos() / 1000 else System.nanoTime() / 1000

    // Algunos fabricantes declaran un reloj y usan otro. Con el primer cuadro se
    // elige el reloj cuya hora queda justo después de la marca de tiempo.
    private fun detectClock(frameTimeMicros: Long) {
        val realtimeDelay = SystemClock.elapsedRealtimeNanos() / 1000 - frameTimeMicros
        val monotonicDelay = System.nanoTime() / 1000 - frameTimeMicros
        val plausible = 0L..2_000_000L
        usesRealtimeClock = when {
            realtimeDelay in plausible && monotonicDelay !in plausible -> true
            monotonicDelay in plausible && realtimeDelay !in plausible -> false
            else -> usesRealtimeClock
        }
        clockDetected = true
    }

    fun cameraIds(): List<String> = cameraManager.cameraIdList.filter { id ->
        val capabilities = cameraManager.getCameraCharacteristics(id)
            .get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
        capabilities.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE)
    }

    fun isFrontCamera(cameraId: String): Boolean =
        cameraManager.getCameraCharacteristics(cameraId).get(CameraCharacteristics.LENS_FACING) ==
            CameraCharacteristics.LENS_FACING_FRONT

    // Tamaño que se usará para esta petición. Si la cámara no tiene exactamente
    // el que pide el PC, se elige el más parecido y el PC lo escala.
    fun chooseSize(cameraId: String, request: StreamRequest): Size {
        val map = cameraManager.getCameraCharacteristics(cameraId)
            .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)!!
        val sizes = map.getOutputSizes(MediaCodec::class.java)
            .filter { it.width % 16 == 0 && it.height % 2 == 0 && it.width <= 1920 && it.height <= 1080 }
        sizes.firstOrNull { it.width == request.width && it.height == request.height }?.let { return it }

        val targetRatio = request.width.toDouble() / request.height
        val targetArea = request.width.toLong() * request.height
        return sizes.minByOrNull { size ->
            val ratioPenalty = abs(size.width.toDouble() / size.height - targetRatio) * 10
            val areaPenalty = abs(size.width.toLong() * size.height - targetArea).toDouble() / targetArea
            ratioPenalty + areaPenalty
        } ?: Size(request.width, request.height)
    }

    // Ajustes de captura que afectan la latencia. Se pueden cambiar para las
    // pruebas al abrir la app desde adb (ver MainActivity).
    data class Options(
        // Vista previa: menos procesado que la plantilla de grabación
        val previewTemplate: Boolean = true,
        // La estabilización retiene cuadros para compararlos
        val stabilization: Boolean = false,
        // La reducción de ruido de alta calidad puede retener cuadros
        val fastNoiseReduction: Boolean = true,
        // Mostrar la imagen también en la pantalla del celular
        val showPreview: Boolean = true,
        // Ajustes de baja latencia propios de los codificadores MediaTek
        val mtkLowLatency: Boolean = false,
        val mtkWifiDisplay: Boolean = false,
        val mtkNoPreprocessing: Boolean = false,
    )

    var options = Options()

    @SuppressLint("MissingPermission")
    fun start(cameraId: String, request: StreamRequest, preview: SurfaceTexture?) {
        stop()
        running = true
        Log.i(TAG, "Opciones: $options")

        val characteristics = cameraManager.getCameraCharacteristics(cameraId)
        usesRealtimeClock = characteristics.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
            CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
        clockDetected = false
        encoderDelayMicros = 0
        cameraDelayMicros = 0

        val size = chooseSize(cameraId, request)
        val frameRate = chooseFrameRate(characteristics, request.frameRate)

        try {
            startEncoder(size, frameRate.upper, request.bitrate)
        } catch (error: Exception) {
            listener.onError("No se pudo iniciar el codificador: ${error.message}")
            stop()
            return
        }

        previewSurface = preview?.takeIf { options.showPreview }?.let {
            it.setDefaultBufferSize(size.width, size.height)
            Surface(it)
        }

        val thread = HandlerThread("CamSolidCamera").apply { start() }
        cameraThread = thread
        cameraHandler = Handler(thread.looper)

        cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                if (!running) {
                    device.close()
                    return
                }
                camera = device
                createSession(device, frameRate, size)
            }

            override fun onDisconnected(device: CameraDevice) {
                device.close()
                listener.onError("Otra aplicación tomó la cámara")
            }

            override fun onError(device: CameraDevice, error: Int) {
                device.close()
                listener.onError("Error de cámara $error")
            }
        }, cameraHandler)
    }

    // Prefiere un rango fijo (por ejemplo 30-30): la exposición no alarga los cuadros
    private fun chooseFrameRate(characteristics: CameraCharacteristics, wanted: Int): Range<Int> {
        val ranges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return Range(wanted, wanted)
        return ranges.firstOrNull { it.lower == wanted && it.upper == wanted }
            ?: ranges.filter { it.upper == wanted }.maxByOrNull { it.lower }
            ?: ranges.minByOrNull { abs(it.upper - wanted) }
            ?: Range(wanted, wanted)
    }

    private fun startEncoder(size: Size, frameRate: Int, bitrate: Int) {
        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)

        // Cada codificador acepta ajustes distintos: se prueba de la configuración
        // más completa a la mínima y se usa la primera que acepte
        val attempts = listOf(
            encoderFormat(size, frameRate, bitrate, baseline = true, lowLatency = true, prependHeaders = true),
            encoderFormat(size, frameRate, bitrate, baseline = true, lowLatency = true, prependHeaders = false),
            encoderFormat(size, frameRate, bitrate, baseline = false, lowLatency = true, prependHeaders = false),
            encoderFormat(size, frameRate, bitrate, baseline = false, lowLatency = false, prependHeaders = false),
        )
        var lastError: Exception? = null
        val configured = attempts.any { format ->
            try {
                codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                true
            } catch (error: Exception) {
                lastError = error
                codec.reset()
                false
            }
        }
        if (!configured) {
            codec.release()
            throw lastError ?: IllegalStateException("configuración rechazada")
        }

        logEncoder(codec)
        encoderSurface = codec.createInputSurface()
        codec.start()
        encoder = codec

        outputThread = Thread({ drainEncoder(codec) }, "CamSolidEncoder").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun encoderFormat(
        size: Size, frameRate: Int, bitrate: Int,
        baseline: Boolean, lowLatency: Boolean, prependHeaders: Boolean,
    ): MediaFormat = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, size.width, size.height).apply {
        setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
        setInteger(MediaFormat.KEY_FRAME_RATE, frameRate)
        setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)

        // Perfil Baseline: nunca usa cuadros B, que obligan a esperar
        if (baseline) {
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
            setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel41)
        }
        // Tiempo real: el codificador entrega cada cuadro sin esperar a los siguientes
        if (lowLatency) {
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger("latency", 1)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            }
            // Ajuste propio de los chips Qualcomm; los demás lo ignoran
            setInteger("vendor.qti-ext-enc-low-latency.enable", 1)
            if (options.mtkLowLatency) setInteger("vendor.mtk.ext.venc.wfd.low-latency-enabled", 1)
            if (options.mtkWifiDisplay) setInteger("vendor.mtk.ext.venc.wfd.feature-on", 1)
            if (options.mtkNoPreprocessing) setInteger("vendor.mtk.ext.venc.vpp.disabled.value", 1)
        }
        // Repite SPS y PPS en cada cuadro clave, por si el PC se reconecta a mitad
        if (prependHeaders && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
        }
    }

    private fun drainEncoder(codec: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val index = try {
                codec.dequeueOutputBuffer(info, 50_000)
            } catch (error: IllegalStateException) {
                return
            }
            if (index < 0) continue

            val buffer = codec.getOutputBuffer(index)
            if (buffer != null && info.size > 0) {
                val data = ByteArray(info.size)
                buffer.position(info.offset)
                buffer.get(data)
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                    listener.onCodecConfig(data)
                } else {
                    if (!clockDetected) detectClock(info.presentationTimeUs)
                    val delay = clockMicros() - info.presentationTimeUs
                    encoderDelayMicros = (encoderDelayMicros * 15 + delay) / 16
                    val keyFrame = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                    listener.onFrame(data, info.presentationTimeUs, keyFrame)
                }
            }
            try {
                codec.releaseOutputBuffer(index, false)
            } catch (error: IllegalStateException) {
                return
            }
        }
    }

    // Mide cuánto tarda la cámara desde que captura hasta que entrega el cuadro
    private val captureCallback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(session: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
            val sensorTime = result.get(CaptureResult.SENSOR_TIMESTAMP) ?: return
            // Android puede pasar las marcas de tiempo a otro reloj antes de dárselas
            // al codificador, así que aquí también se elige el reloj que tenga sentido
            val realtimeDelay = SystemClock.elapsedRealtimeNanos() / 1000 - sensorTime / 1000
            val monotonicDelay = System.nanoTime() / 1000 - sensorTime / 1000
            val delay = listOf(realtimeDelay, monotonicDelay).filter { it in 0L..2_000_000L }.minOrNull() ?: return
            cameraDelayMicros = if (cameraDelayMicros == 0L) delay else (cameraDelayMicros * 15 + delay) / 16
            result.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { exposureMicros = it / 1000 }
        }
    }

    // Anota en el registro el codificador elegido y sus ajustes propios de latencia
    private fun logEncoder(codec: MediaCodec) {
        encoderName = codec.name
        Log.i(TAG, "Codificador: ${codec.name}")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // En varias líneas: logcat corta las líneas muy largas
            codec.supportedVendorParameters.chunked(12).forEach { Log.i(TAG, "Ajustes del fabricante: ${it.joinToString()}") }
        }
    }

    @Suppress("DEPRECATION")
    private fun createSession(device: CameraDevice, frameRate: Range<Int>, size: Size) {
        val targets = listOfNotNull(encoderSurface, previewSurface)
        device.createCaptureSession(targets, object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(captureSession: CameraCaptureSession) {
                if (!running) return
                session = captureSession
                val template = if (options.previewTemplate) CameraDevice.TEMPLATE_PREVIEW else CameraDevice.TEMPLATE_RECORD
                val request = device.createCaptureRequest(template).apply {
                    targets.forEach { addTarget(it) }
                    set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, frameRate)
                    set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                    set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                    if (!options.stabilization) {
                        set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
                        set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_OFF)
                    }
                    if (options.fastNoiseReduction) {
                        set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST)
                        set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_FAST)
                    }
                }
                try {
                    captureSession.setRepeatingRequest(request.build(), captureCallback, cameraHandler)
                    listener.onStarted(size, frameRate.upper)
                } catch (error: Exception) {
                    listener.onError("No se pudo iniciar la captura: ${error.message}")
                }
            }

            override fun onConfigureFailed(captureSession: CameraCaptureSession) {
                listener.onError("La cámara no aceptó la configuración")
            }
        }, cameraHandler)
    }

    // Pide al codificador un cuadro clave para el siguiente cuadro
    fun requestKeyFrame() {
        try {
            encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (_: IllegalStateException) {
        }
    }

    fun stop() {
        running = false
        try { session?.close() } catch (_: Exception) {}
        session = null
        try { camera?.close() } catch (_: Exception) {}
        camera = null
        outputThread?.join(500)
        outputThread = null
        try { encoder?.stop() } catch (_: Exception) {}
        try { encoder?.release() } catch (_: Exception) {}
        encoder = null
        encoderSurface?.release()
        encoderSurface = null
        previewSurface?.release()
        previewSurface = null
        cameraThread?.quitSafely()
        cameraThread = null
        cameraHandler = null
    }
}
