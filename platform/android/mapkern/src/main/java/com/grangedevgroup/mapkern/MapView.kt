package com.grangedevgroup.mapkern

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.opengl.GLSurfaceView
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.view.MotionEvent
import java.io.File
import java.util.concurrent.CopyOnWriteArrayList
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

data class MapOptions(
    val style: String = "https://tiles.openfreemap.org/styles/liberty",
    val longitude: Double = 0.0,
    val latitude: Double = 0.0,
    val zoom: Double = 0.0,
    val globe: Boolean = true,
    val controls: Boolean = false,
)

data class Camera(val longitude: Double, val latitude: Double, val zoom: Double, val bearing: Double, val pitch: Double)

class MapView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    options: MapOptions = MapOptions(),
) : GLSurfaceView(context, attrs) {
    private var handle: Long = 0
    private var surfaceWidth = 0
    private var surfaceHeight = 0
    private val density = resources.displayMetrics.density.toDouble()
    private val main = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArrayList<(String, String) -> Unit>()
    private val connectivity = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) = updateReachability()
        override fun onLost(network: Network) = updateReachability()
        override fun onCapabilitiesChanged(network: Network, capabilities: NetworkCapabilities) = updateReachability()
    }
    init {
        System.loadLibrary("mapkern-jni")
        setEGLContextClientVersion(3)
        setEGLConfigChooser(8, 8, 8, 8, 24, 8)
        preserveEGLContextOnPause = true
        val cache = File(context.cacheDir, "mapkern").absolutePath
        handle = nativeCreate(options.style, cache, options.longitude, options.latitude, options.zoom, options.globe, density, options.controls)
        setRenderer(object : Renderer {
            override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
                if (handle != 0L) nativeSurfaceCreated(handle)
            }

            override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
                surfaceWidth = width
                surfaceHeight = height
                if (handle != 0L) nativeResize(handle, width, height, density)
            }

            override fun onDrawFrame(gl: GL10?) {
                if (handle == 0L) return
                if (nativeRender(handle, surfaceWidth, surfaceHeight)) requestRender()
            }
        })
        renderMode = RENDERMODE_WHEN_DIRTY
        try {
            connectivity?.registerDefaultNetworkCallback(networkCallback)
        } catch (e: RuntimeException) {
        }
        updateReachability()
    }

    private fun updateReachability() {
        val cm = connectivity ?: return
        val caps = cm.getNetworkCapabilities(cm.activeNetwork)
        nativeSetReachable(caps?.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) == true)
        requestRender()
    }

    fun addListener(listener: (type: String, message: String) -> Unit) {
        listeners.add(listener)
    }

    fun setStyle(style: String) = queueEvent { if (handle != 0L) nativeSetStyle(handle, style) }

    fun flyTo(longitude: Double, latitude: Double, zoom: Double, bearing: Double = 0.0, pitch: Double = 0.0) =
        queueEvent { if (handle != 0L) nativeFlyTo(handle, longitude, latitude, zoom, bearing, pitch) }

    fun setGlobe(globe: Boolean) = queueEvent { if (handle != 0L) nativeSetProjection(handle, globe) }

    fun setView(view: String) = queueEvent { if (handle != 0L) nativeSetView(handle, view) }

    fun selectBasemap(index: Int) = queueEvent { if (handle != 0L) nativeSelectBasemap(handle, index) }

    fun setOffline(offline: Boolean) = queueEvent { if (handle != 0L) nativeSetOffline(handle, offline) }

    fun zoomBy(delta: Double) = queueEvent {
        if (handle != 0L) nativeZoomBy(handle, delta, surfaceWidth / density / 2, surfaceHeight / density / 2)
    }

    fun camera(): Camera? {
        if (handle == 0L) return null
        val c = nativeCamera(handle)
        return Camera(c[0], c[1], c[2], c[3], c[4])
    }

    fun diagnostics(callback: (String) -> Unit) = queueEvent {
        callback(if (handle != 0L) String(nativeDiagnostics(handle), Charsets.UTF_8) else "{}")
    }

    fun graphicsInfo(callback: (String) -> Unit) = queueEvent { callback(String(nativeGlInfo(), Charsets.UTF_8)) }

    fun setGpuTiming(enabled: Boolean) = queueEvent { if (handle != 0L) nativeSetGpuTiming(handle, enabled) }

    fun setHud(lines: List<String>) {
        val text = lines.joinToString("\n")
        queueEvent { if (handle != 0L) nativeSetHud(handle, text) }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val action = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> 0
            MotionEvent.ACTION_MOVE -> 1
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> 2
            MotionEvent.ACTION_CANCEL -> 3
            else -> return true
        }
        val points = if (action == 1 || action == 3) (0 until event.pointerCount).toList() else listOf(event.actionIndex)
        val ids = points.map { event.getPointerId(it) }
        val xs = points.map { event.getX(it) / density }
        val ys = points.map { event.getY(it) / density }
        val time = event.eventTime.toDouble()
        queueEvent {
            if (handle == 0L) return@queueEvent
            for (i in ids.indices) nativePointer(handle, action, ids[i], xs[i], ys[i], time)
        }
        requestRender()
        return true
    }

    @Suppress("unused")
    private fun onNativeEvent(type: String, message: String) {
        main.post { for (l in listeners) l(type, message) }
    }

    fun destroy() {
        try {
            connectivity?.unregisterNetworkCallback(networkCallback)
        } catch (e: RuntimeException) {
        }
        val h = handle
        handle = 0
        if (h != 0L) queueEvent { nativeDestroy(h) }
    }

    private external fun nativeCreate(style: String, cache: String, lng: Double, lat: Double, zoom: Double, globe: Boolean, ratio: Double, controls: Boolean): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeSurfaceCreated(handle: Long)
    private external fun nativeResize(handle: Long, width: Int, height: Int, ratio: Double)
    private external fun nativeRender(handle: Long, width: Int, height: Int): Boolean
    private external fun nativePointer(handle: Long, action: Int, id: Int, x: Double, y: Double, time: Double)
    private external fun nativeZoomBy(handle: Long, delta: Double, x: Double, y: Double)
    private external fun nativeSetStyle(handle: Long, style: String)
    private external fun nativeFlyTo(handle: Long, lng: Double, lat: Double, zoom: Double, bearing: Double, pitch: Double)
    private external fun nativeSetProjection(handle: Long, globe: Boolean)
    private external fun nativeSetOffline(handle: Long, offline: Boolean)
    private external fun nativeCamera(handle: Long): DoubleArray
    private external fun nativeDiagnostics(handle: Long): ByteArray
    private external fun nativeGlInfo(): ByteArray
    private external fun nativeSetGpuTiming(handle: Long, enabled: Boolean)
    private external fun nativeSetHud(handle: Long, text: String)

    companion object {
        @JvmStatic private external fun nativeSetReachable(reachable: Boolean)
    }
}
