package com.grangedevgroup.mapkern

import android.app.Activity
import android.app.Application
import android.content.Context
import android.content.ContextWrapper
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.opengl.GLSurfaceView
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.view.MotionEvent
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicLong
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

data class MapOptions(
    val style: String = "https://tiles.openfreemap.org/styles/liberty",
    val longitude: Double = 0.0,
    val latitude: Double = 0.0,
    val zoom: Double = 0.0,
    val bearing: Double = 0.0,
    val pitch: Double = 0.0,
    val globe: Boolean = true,
    val controls: Boolean = false,
)

data class MapClick(val longitude: Double, val latitude: Double, val x: Double, val y: Double)

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
    private val clickListeners = CopyOnWriteArrayList<(MapClick) -> Unit>()
    private val downloads = ConcurrentHashMap<Long, (Long, Long) -> Unit>()
    private val nextDownload = AtomicLong(1)
    private val host: Activity? = generateSequence(context) { (it as? ContextWrapper)?.baseContext }.filterIsInstance<Activity>().firstOrNull()
    private val lifecycle = object : Application.ActivityLifecycleCallbacks {
        override fun onActivityCreated(activity: Activity, state: Bundle?) {}
        override fun onActivityStarted(activity: Activity) {}
        override fun onActivityResumed(activity: Activity) {
            if (activity === host) onResume()
        }
        override fun onActivityPaused(activity: Activity) {
            if (activity === host) onPause()
        }
        override fun onActivityStopped(activity: Activity) {}
        override fun onActivitySaveInstanceState(activity: Activity, state: Bundle) {}
        override fun onActivityDestroyed(activity: Activity) {
            if (activity === host) destroy()
        }
    }
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
        handle = nativeCreate(options.style, cache, options.longitude, options.latitude, options.zoom, options.globe, density, options.controls, options.bearing, options.pitch)
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
        host?.application?.registerActivityLifecycleCallbacks(lifecycle)
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

    fun onClick(listener: (MapClick) -> Unit) {
        clickListeners.add(listener)
    }

    fun setStyle(style: String) = queueEvent { if (handle != 0L) nativeSetStyle(handle, style) }

    fun addSource(id: String, json: String) = queueEvent { if (handle != 0L) nativeAddSource(handle, id, json) }

    fun setGeoJson(sourceId: String, json: String) = queueEvent { if (handle != 0L) nativeSetGeoJson(handle, sourceId, json) }

    fun removeSource(id: String) = queueEvent { if (handle != 0L) nativeRemoveSource(handle, id) }

    fun addLayer(json: String, before: String? = null) = queueEvent { if (handle != 0L) nativeAddLayer(handle, json, before ?: "") }

    fun removeLayer(id: String) = queueEvent { if (handle != 0L) nativeRemoveLayer(handle, id) }

    fun setPaintProperty(layer: String, name: String, json: String) =
        queueEvent { if (handle != 0L) nativeSetPaintProperty(handle, layer, name, json) }

    fun setLayoutProperty(layer: String, name: String, json: String) =
        queueEvent { if (handle != 0L) nativeSetLayoutProperty(handle, layer, name, json) }

    fun setFilter(layer: String, json: String?) = queueEvent { if (handle != 0L) nativeSetFilter(handle, layer, json ?: "") }

    fun setLayerVisible(layer: String, visible: Boolean) = queueEvent { if (handle != 0L) nativeSetLayerVisible(handle, layer, visible) }

    fun setTerrain(source: String, exaggeration: Double = 1.0, relief: Double = 0.5) =
        queueEvent { if (handle != 0L) nativeSetTerrain(handle, source, exaggeration, relief) }

    fun clearTerrain() = queueEvent { if (handle != 0L) nativeClearTerrain(handle) }

    fun flyTo(longitude: Double, latitude: Double, zoom: Double, bearing: Double = Double.NaN, pitch: Double = Double.NaN) =
        queueEvent { if (handle != 0L) nativeFlyTo(handle, longitude, latitude, zoom, bearing, pitch) }

    fun easeTo(longitude: Double, latitude: Double, zoom: Double, bearing: Double = Double.NaN, pitch: Double = Double.NaN) =
        queueEvent { if (handle != 0L) nativeEaseTo(handle, longitude, latitude, zoom, bearing, pitch) }

    fun jumpTo(longitude: Double, latitude: Double, zoom: Double, bearing: Double = Double.NaN, pitch: Double = Double.NaN) =
        queueEvent { if (handle != 0L) nativeJumpTo(handle, longitude, latitude, zoom, bearing, pitch) }

    fun project(longitude: Double, latitude: Double, callback: (x: Double, y: Double) -> Unit) = queueEvent {
        if (handle != 0L) {
            val p = nativeProject(handle, longitude, latitude)
            main.post { callback(p[0], p[1]) }
        }
    }

    fun unproject(x: Double, y: Double, callback: (longitude: Double, latitude: Double) -> Unit) = queueEvent {
        if (handle != 0L) nativeUnproject(handle, x, y)?.let { p -> main.post { callback(p[0], p[1]) } }
    }

    fun queryFeatures(x: Double, y: Double, layers: List<String> = emptyList(), radius: Double = 8.0, callback: (JSONArray) -> Unit) = queueEvent {
        val json = if (handle != 0L) String(nativeQueryFeatures(handle, x, y, layers.toTypedArray(), radius), Charsets.UTF_8) else "[]"
        main.post { callback(JSONArray(json)) }
    }

    fun clearDiskCache() = queueEvent { if (handle != 0L) nativeClearDiskCache(handle) }

    fun downloadRegion(
        west: Double,
        south: Double,
        east: Double,
        north: Double,
        minZoom: Int,
        maxZoom: Int,
        progress: ((done: Long, total: Long) -> Unit)? = null,
    ) {
        val request = nextDownload.getAndIncrement()
        if (progress != null) downloads[request] = progress
        queueEvent { if (handle != 0L) nativeDownloadRegion(handle, west, south, east, north, minZoom, maxZoom, request) }
    }

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
        main.post {
            for (l in listeners) l(type, message)
            if (type == "click") {
                val o = JSONObject(message)
                val click = MapClick(o.getDouble("lng"), o.getDouble("lat"), o.getDouble("x"), o.getDouble("y"))
                for (l in clickListeners) l(click)
            }
        }
    }

    @Suppress("unused")
    private fun onNativeProgress(request: Long, done: Long, total: Long) {
        val listener = downloads[request] ?: return
        if (done >= total) downloads.remove(request)
        main.post { listener(done, total) }
    }

    fun destroy() {
        host?.application?.unregisterActivityLifecycleCallbacks(lifecycle)
        try {
            connectivity?.unregisterNetworkCallback(networkCallback)
        } catch (e: RuntimeException) {
        }
        val h = handle
        handle = 0
        if (h != 0L) queueEvent { nativeDestroy(h) }
    }

    private external fun nativeCreate(style: String, cache: String, lng: Double, lat: Double, zoom: Double, globe: Boolean, ratio: Double, controls: Boolean, bearing: Double, pitch: Double): Long
    private external fun nativeEaseTo(handle: Long, lng: Double, lat: Double, zoom: Double, bearing: Double, pitch: Double)
    private external fun nativeJumpTo(handle: Long, lng: Double, lat: Double, zoom: Double, bearing: Double, pitch: Double)
    private external fun nativeAddSource(handle: Long, id: String, json: String)
    private external fun nativeSetGeoJson(handle: Long, id: String, json: String)
    private external fun nativeRemoveSource(handle: Long, id: String)
    private external fun nativeAddLayer(handle: Long, json: String, before: String)
    private external fun nativeRemoveLayer(handle: Long, id: String)
    private external fun nativeSetPaintProperty(handle: Long, layer: String, name: String, value: String)
    private external fun nativeSetLayoutProperty(handle: Long, layer: String, name: String, value: String)
    private external fun nativeSetFilter(handle: Long, layer: String, filter: String)
    private external fun nativeSetLayerVisible(handle: Long, layer: String, visible: Boolean)
    private external fun nativeSetTerrain(handle: Long, source: String, exaggeration: Double, relief: Double)
    private external fun nativeClearTerrain(handle: Long)
    private external fun nativeProject(handle: Long, lng: Double, lat: Double): DoubleArray
    private external fun nativeUnproject(handle: Long, x: Double, y: Double): DoubleArray?
    private external fun nativeQueryFeatures(handle: Long, x: Double, y: Double, layers: Array<String>, radius: Double): ByteArray
    private external fun nativeClearDiskCache(handle: Long)
    private external fun nativeDownloadRegion(handle: Long, west: Double, south: Double, east: Double, north: Double, minZoom: Int, maxZoom: Int, request: Long)
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
    private external fun nativeSetView(handle: Long, view: String)
    private external fun nativeSelectBasemap(handle: Long, index: Int)

    companion object {
        @JvmStatic private external fun nativeSetReachable(reachable: Boolean)
    }
}
