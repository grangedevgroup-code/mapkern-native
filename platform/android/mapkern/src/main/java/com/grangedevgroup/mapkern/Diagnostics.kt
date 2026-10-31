package com.grangedevgroup.mapkern

import android.app.ActivityManager
import android.content.ComponentCallbacks2
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.Configuration
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.TrafficStats
import android.os.BatteryManager
import android.os.Build
import android.os.Debug
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.os.Process
import android.os.StatFs
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import android.util.Log
import android.view.WindowManager
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean

data class DiagnosticsOptions(
    val intervalMs: Long = 1000,
    val logcat: Boolean = true,
    val file: Boolean = true,
    val hud: Boolean = false,
    val gpuTiming: Boolean = true,
)

class Diagnostics(context: Context, private val map: MapView, private val options: DiagnosticsOptions = DiagnosticsOptions()) {
    private val app = context.applicationContext
    private val thread = HandlerThread("MapkernDiag").apply { start() }
    private val handler = Handler(thread.looper)
    private val power = app.getSystemService(Context.POWER_SERVICE) as PowerManager
    private val activity = app.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
    private val battery = app.getSystemService(Context.BATTERY_SERVICE) as BatteryManager
    private val connectivity = app.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val uid = Process.myUid()
    private val cores = Runtime.getRuntime().availableProcessors()
    private val ticks = Os.sysconf(OsConstants._SC_CLK_TCK).coerceAtLeast(1)
    private val running = AtomicBoolean(false)
    private val waiting = AtomicBoolean(false)
    private var seq = 0L
    private var lastWall = SystemClock.elapsedRealtime()
    private var lastCpu = Process.getElapsedCpuTime()
    private var lastRx = TrafficStats.getUidRxBytes(uid)
    private var lastTx = TrafficStats.getUidTxBytes(uid)
    private var lastThreads = HashMap<Int, Long>()
    private var cpuFreqReadable = true
    private var pss = JSONObject()
    private var graphics: JSONObject? = null
    private var foreground = true
    private var writer: FileWriter? = null
    private var written = 0L
    private var lastEngine: JSONObject? = null
    private var previousHandler: Thread.UncaughtExceptionHandler? = null

    var file: File? = null
        private set

    private val memoryCallbacks = object : ComponentCallbacks2 {
        override fun onTrimMemory(level: Int) = event("trimMemory", JSONObject().put("level", level))
        override fun onConfigurationChanged(newConfig: Configuration) = event("configuration", JSONObject().put("orientation", newConfig.orientation))
        @Deprecated("Deprecated in Android")
        override fun onLowMemory() = event("lowMemory", JSONObject())
    }

    private val thermalListener = PowerManager.OnThermalStatusChangedListener { status -> event("thermal", JSONObject().put("status", status)) }

    private val tick = object : Runnable {
        override fun run() {
            if (!running.get()) return
            sample()
            handler.postDelayed(this, options.intervalMs)
        }
    }

    fun start() {
        if (!running.compareAndSet(false, true)) return
        handler.post {
            openFile()
            event("start", staticInfo())
        }
        app.registerComponentCallbacks(memoryCallbacks)
        if (Build.VERSION.SDK_INT >= 29) power.addThermalStatusListener(handler::post, thermalListener)
        map.addListener { type, message -> if (type == "error") handler.post { event("engineError", JSONObject().put("message", message)) } }
        previousHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { t, e ->
            try {
                line(JSONObject().put("t", System.currentTimeMillis()).put("event", "crash").put("thread", t.name).put("error", Log.getStackTraceString(e)))
                writer?.flush()
            } catch (ignored: Exception) {
            }
            previousHandler?.uncaughtException(t, e)
        }
        if (options.gpuTiming) map.setGpuTiming(true)
        map.graphicsInfo { info -> handler.post { if (info.isNotBlank()) event("graphics", graphicsInfo(info)) } }
        handler.postDelayed(tick, options.intervalMs)
    }

    fun stop() {
        if (!running.compareAndSet(true, false)) return
        try {
            app.unregisterComponentCallbacks(memoryCallbacks)
        } catch (ignored: Exception) {
        }
        if (Build.VERSION.SDK_INT >= 29) power.removeThermalStatusListener(thermalListener)
        Thread.setDefaultUncaughtExceptionHandler(previousHandler)
        map.setGpuTiming(false)
        if (options.hud) map.setHud(emptyList())
        handler.post {
            event("stop", JSONObject())
            writer?.close()
            writer = null
            thread.quitSafely()
        }
    }

    fun setForeground(visible: Boolean) {
        foreground = visible
        handler.post { event(if (visible) "resume" else "pause", JSONObject()) }
    }

    private fun sample() {
        val device = deviceSample()
        if (!waiting.compareAndSet(false, true)) {
            emit(device, null, true)
            return
        }
        map.diagnostics { json ->
            handler.post {
                waiting.set(false)
                emit(device, json, false)
            }
        }
    }

    private fun emit(device: JSONObject, engineJson: String?, stalled: Boolean) {
        val root = JSONObject().put("t", System.currentTimeMillis()).put("seq", seq++).put("device", device)
        if (stalled) root.put("stalled", true)
        if (engineJson != null) {
            try {
                val e = JSONObject(engineJson)
                root.put("engine", e.optJSONObject("engine"))
                root.put("gpu", e.optJSONObject("gpu"))
                lastEngine = e
            } catch (err: Exception) {
                root.put("engineError", err.message)
            }
        }
        line(root)
        if (options.hud) map.setHud(hud(device, lastEngine))
    }

    private fun event(name: String, data: JSONObject) {
        line(JSONObject().put("t", System.currentTimeMillis()).put("event", name).put("data", data))
    }

    private fun line(json: JSONObject) {
        val text = json.toString()
        if (options.logcat) {
            if (text.length <= CHUNK) {
                Log.i(TAG, text)
            } else {
                val parts = (text.length + CHUNK - 1) / CHUNK
                val id = seq
                for (i in 0 until parts) Log.i(TAG, "#$id ${i + 1}/$parts " + text.substring(i * CHUNK, minOf(text.length, (i + 1) * CHUNK)))
            }
        }
        val w = writer ?: return
        try {
            w.write(text)
            w.write("\n")
            written += text.length + 1
            if (seq % 10 == 0L) w.flush()
            if (written > MAX_FILE) {
                w.close()
                openFile()
            }
        } catch (e: Exception) {
            writer = null
        }
    }

    private fun openFile() {
        if (!options.file) return
        val dir = app.getExternalFilesDir("diagnostics") ?: File(app.filesDir, "diagnostics")
        dir.mkdirs()
        dir.listFiles()?.sortedBy { it.lastModified() }?.dropLast(KEEP_FILES)?.forEach { it.delete() }
        val name = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date()) + ".jsonl"
        file = File(dir, name)
        writer = FileWriter(file, true)
        written = 0
    }

    private fun staticInfo(): JSONObject {
        val mi = ActivityManager.MemoryInfo().also { activity.getMemoryInfo(it) }
        val metrics = app.resources.displayMetrics
        val o = JSONObject()
            .put("manufacturer", Build.MANUFACTURER)
            .put("model", Build.MODEL)
            .put("device", Build.DEVICE)
            .put("hardware", Build.HARDWARE)
            .put("android", Build.VERSION.RELEASE)
            .put("sdk", Build.VERSION.SDK_INT)
            .put("abis", JSONArray(Build.SUPPORTED_ABIS.toList()))
            .put("cores", cores)
            .put("ramMb", mi.totalMem / 1048576)
            .put("screen", JSONObject().put("w", metrics.widthPixels).put("h", metrics.heightPixels).put("density", metrics.density).put("dpi", metrics.densityDpi))
            .put("refreshHz", refreshRate())
            .put("package", app.packageName)
        if (Build.VERSION.SDK_INT >= 31) o.put("soc", Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL)
        val maxFreq = JSONArray()
        for (i in 0 until cores) maxFreq.put(readLong("/sys/devices/system/cpu/cpu$i/cpufreq/cpuinfo_max_freq")?.div(1000) ?: JSONObject.NULL)
        o.put("cpuMaxMhz", maxFreq)
        return o
    }

    private fun graphicsInfo(info: String): JSONObject {
        val parts = info.split("\n")
        val extensions = parts.getOrNull(5)?.split(" ")?.filter { it.isNotBlank() } ?: emptyList()
        return JSONObject()
            .put("vendor", parts.getOrNull(0))
            .put("renderer", parts.getOrNull(1))
            .put("version", parts.getOrNull(2))
            .put("shading", parts.getOrNull(3))
            .put("maxTexture", parts.getOrNull(4)?.toIntOrNull())
            .put("extensions", extensions.size)
            .put("timerQuery", extensions.contains("GL_EXT_disjoint_timer_query"))
            .put("astc", extensions.any { it.startsWith("GL_KHR_texture_compression_astc") })
    }

    private fun deviceSample(): JSONObject {
        val now = SystemClock.elapsedRealtime()
        val wall = (now - lastWall).coerceAtLeast(1)
        lastWall = now
        val o = JSONObject().put("foreground", foreground)

        val b = app.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (b != null) {
            val level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100).coerceAtLeast(1)
            val voltage = b.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0)
            val current = battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
            val bat = JSONObject()
                .put("pct", level * 100 / scale)
                .put("tempC", b.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0)
                .put("mV", voltage)
                .put("uA", current)
                .put("avgUA", battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_AVERAGE))
                .put("status", b.getIntExtra(BatteryManager.EXTRA_STATUS, -1))
                .put("plugged", b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0))
            if (current != Int.MIN_VALUE && voltage > 0) bat.put("watts", round(voltage / 1000.0 * current / 1_000_000.0, 3))
            o.put("battery", bat)
        }

        val thermal = JSONObject()
        if (Build.VERSION.SDK_INT >= 29) thermal.put("status", power.currentThermalStatus)
        if (Build.VERSION.SDK_INT >= 30) {
            val headroom = power.getThermalHeadroom(10)
            if (!headroom.isNaN()) thermal.put("headroom", round(headroom.toDouble(), 3))
        }
        thermal.put("powerSave", power.isPowerSaveMode)
        o.put("thermal", thermal)

        val cpuNow = Process.getElapsedCpuTime()
        val cpu = JSONObject().put("appPct", round((cpuNow - lastCpu) * 100.0 / wall / cores, 1))
        lastCpu = cpuNow
        cpu.put("threads", threadSample(wall))
        if (cpuFreqReadable) {
            val freqs = JSONArray()
            for (i in 0 until cores) {
                val f = readLong("/sys/devices/system/cpu/cpu$i/cpufreq/scaling_cur_freq")
                if (f == null && i == 0) {
                    cpuFreqReadable = false
                    break
                }
                freqs.put(f?.div(1000) ?: JSONObject.NULL)
            }
            if (cpuFreqReadable) cpu.put("mhz", freqs)
        }
        o.put("cpu", cpu)

        val mi = ActivityManager.MemoryInfo().also { activity.getMemoryInfo(it) }
        val rt = Runtime.getRuntime()
        val mem = JSONObject()
            .put("availMb", mi.availMem / 1048576)
            .put("totalMb", mi.totalMem / 1048576)
            .put("low", mi.lowMemory)
            .put("nativeMb", round(Debug.getNativeHeapAllocatedSize() / 1048576.0, 1))
            .put("javaMb", round((rt.totalMemory() - rt.freeMemory()) / 1048576.0, 1))
        if (seq % 5 == 0L) {
            val d = Debug.MemoryInfo()
            Debug.getMemoryInfo(d)
            pss = JSONObject()
                .put("totalMb", round(d.totalPss / 1024.0, 1))
                .put("graphicsMb", round((d.getMemoryStat("summary.graphics")?.toDoubleOrNull() ?: 0.0) / 1024.0, 1))
                .put("nativeMb", round((d.getMemoryStat("summary.native-heap")?.toDoubleOrNull() ?: 0.0) / 1024.0, 1))
                .put("javaMb", round((d.getMemoryStat("summary.java-heap")?.toDoubleOrNull() ?: 0.0) / 1024.0, 1))
                .put("codeMb", round((d.getMemoryStat("summary.code")?.toDoubleOrNull() ?: 0.0) / 1024.0, 1))
        }
        mem.put("pss", pss)
        o.put("memory", mem)

        val rx = TrafficStats.getUidRxBytes(uid)
        val tx = TrafficStats.getUidTxBytes(uid)
        val net = JSONObject()
        if (rx >= 0 && lastRx >= 0) net.put("rxKbps", round((rx - lastRx) * 8.0 / wall, 1)).put("rxBytes", rx - lastRx)
        if (tx >= 0 && lastTx >= 0) net.put("txKbps", round((tx - lastTx) * 8.0 / wall, 1))
        lastRx = rx
        lastTx = tx
        val caps = connectivity.activeNetwork?.let { connectivity.getNetworkCapabilities(it) }
        if (caps != null) {
            net.put("type", when {
                caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "wifi"
                caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "cellular"
                caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "ethernet"
                caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN) -> "vpn"
                else -> "other"
            })
            net.put("downKbps", caps.linkDownstreamBandwidthKbps).put("upKbps", caps.linkUpstreamBandwidthKbps)
            net.put("validated", caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED))
            if (Build.VERSION.SDK_INT >= 29) net.put("signal", caps.signalStrength)
        } else {
            net.put("type", "none")
        }
        net.put("metered", connectivity.isActiveNetworkMetered)
        o.put("network", net)

        o.put("display", JSONObject().put("hz", refreshRate()))
        try {
            o.put("storage", JSONObject().put("freeMb", StatFs(app.cacheDir.path).availableBytes / 1048576))
        } catch (ignored: Exception) {
        }
        return o
    }

    private fun threadSample(wall: Long): JSONArray {
        val now = HashMap<Int, Long>()
        val names = HashMap<Int, String>()
        File("/proc/self/task").listFiles()?.forEach { task ->
            val tid = task.name.toIntOrNull() ?: return@forEach
            val stat = try {
                File(task, "stat").readText()
            } catch (e: Exception) {
                return@forEach
            }
            val open = stat.indexOf('(')
            val close = stat.lastIndexOf(')')
            if (open < 0 || close < open) return@forEach
            val fields = stat.substring(close + 2).split(' ')
            if (fields.size < 13) return@forEach
            now[tid] = (fields[11].toLongOrNull() ?: 0) + (fields[12].toLongOrNull() ?: 0)
            names[tid] = stat.substring(open + 1, close)
        }
        val busy = now.mapNotNull { (tid, t) ->
            val before = lastThreads[tid] ?: return@mapNotNull null
            val ms = (t - before) * 1000.0 / ticks
            if (ms <= 0) null else Triple(tid, names[tid] ?: "?", ms)
        }.sortedByDescending { it.third }.take(6)
        lastThreads = now
        val out = JSONArray()
        for ((_, name, ms) in busy) out.put(JSONObject().put("name", name).put("pct", round(ms * 100.0 / wall, 1)))
        return out
    }

    private fun hud(device: JSONObject, engine: JSONObject?): List<String> {
        val e = engine?.optJSONObject("engine")
        val gpu = engine?.optJSONObject("gpu")
        val frame = e?.optJSONObject("frame")
        val net = e?.optJSONObject("net")
        val cache = e?.optJSONObject("cache")
        val camera = e?.optJSONObject("camera")
        val errors = e?.optJSONObject("errors")
        val bat = device.optJSONObject("battery")
        val thermal = device.optJSONObject("thermal")
        val mem = device.optJSONObject("memory")
        val cpu = device.optJSONObject("cpu")
        var loading = 0
        var blank = 0.0
        e?.optJSONObject("tiles")?.let { t -> t.keys().forEach { k -> t.optJSONObject(k)?.let { loading += it.optInt("loading"); blank = maxOf(blank, it.optDouble("blank", 0.0)) } } }
        val gpuText = if (gpu?.optBoolean("timer") == true) "gpu ${fmt(gpu.optDouble("avgMs"))}/${fmt(gpu.optDouble("maxMs"))}ms" else "gpu n/a"
        return listOf(
            "fps ${fmt(frame?.optDouble("fps"))}  cpu ${fmt(frame?.optDouble("cpuAvgMs"))}/${fmt(frame?.optDouble("cpuMaxMs"))}ms  $gpuText",
            "frames ${frame?.optInt("frames")}  slow ${frame?.optInt("slow")}  gap max ${fmt(frame?.optDouble("intervalMaxMs"))}ms  draws ${frame?.optInt("drawsAvg")}",
            "tiles loading $loading  blank ${fmt(blank * 100)}%  ${fmt(e?.optDouble("tileMb"))}MB",
            "net ${fmt(net?.optDouble("kbps"))}kbps  lat ${fmt(net?.optDouble("latencyAvgMs"))}ms  q ${net?.optInt("queued")}/${net?.optInt("active")}  cache ${cache?.optInt("hits")}",
            "app cpu ${fmt(cpu?.optDouble("appPct"))}%  therm ${thermal?.optInt("status", -1)} head ${fmt(thermal?.optDouble("headroom"))}  bat ${fmt(bat?.optDouble("tempC"))}C",
            "power ${fmt(bat?.optDouble("watts"))}W  bat ${bat?.optInt("pct")}%  native ${fmt(mem?.optDouble("nativeMb"))}MB  pss ${fmt(mem?.optJSONObject("pss")?.optDouble("totalMb"))}MB",
            "z ${fmt(camera?.optDouble("zoom"))}  ${camera?.optString("view")}  globe ${fmt(camera?.optDouble("globe"))}  errors ${errors?.optInt("total")}",
        )
    }

    private fun refreshRate(): Float {
        return try {
            if (Build.VERSION.SDK_INT >= 30) app.display?.refreshRate ?: 0f
            else @Suppress("DEPRECATION") (app.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.refreshRate
        } catch (e: Exception) {
            0f
        }
    }

    private fun readLong(path: String): Long? = try {
        File(path).readText().trim().toLongOrNull()
    } catch (e: Exception) {
        null
    }

    private fun fmt(v: Double?): String = if (v == null || v.isNaN()) "-" else if (v >= 100) "%.0f".format(Locale.US, v) else "%.1f".format(Locale.US, v)

    private fun round(v: Double, digits: Int): Double {
        val k = Math.pow(10.0, digits.toDouble())
        return Math.round(v * k) / k
    }

    companion object {
        const val TAG = "MapkernDiag"
        private const val CHUNK = 3500
        private const val MAX_FILE = 32L * 1024 * 1024
        private const val KEEP_FILES = 6
    }
}
