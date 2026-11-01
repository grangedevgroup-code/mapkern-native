package com.grangedevgroup.mapkern.demo

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import com.grangedevgroup.mapkern.MapView
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.cos
import kotlin.math.sin

class Bench(private val view: View, private val done: () -> Unit = {}) {
    private val handler = Handler(Looper.getMainLooper())
    private var at = 0L

    companion object {
        const val TAG = "MapkernBench"
    }

    fun gestures() {
        if (view.width == 0 || view.height == 0) {
            handler.postDelayed({ gestures() }, 200)
            return
        }
        at = SystemClock.uptimeMillis() + 3000
        val w = view.width.toFloat()
        val h = view.height.toFloat()
        val cx = w / 2
        val cy = h / 2
        repeat(6) {
            pinch(cx, cy, w * 0.12f, w * 0.62f, 520)
            pause(700)
        }
        drag(cx, cy - h * 0.2f, cx, cy + h * 0.2f, 600)
        pause(600)
        drag(cx + w * 0.3f, cy, cx - w * 0.3f, cy, 600)
        pause(600)
        twist(cx, cy, w * 0.45f, 50.0, 700)
        pause(800)
        tilt(cx, cy + h * 0.15f, w * 0.35f, -h * 0.25f, 700)
        pause(800)
        drag(cx, cy + h * 0.2f, cx, cy - h * 0.2f, 600)
        pause(600)
        tilt(cx, cy - h * 0.1f, w * 0.35f, h * 0.25f, 700)
        pause(800)
        twist(cx, cy, w * 0.45f, -50.0, 700)
        pause(800)
        repeat(6) {
            pinch(cx, cy, w * 0.62f, w * 0.12f, 520)
            pause(700)
        }
        handler.postAtTime(done, at)
    }

    fun globe() {
        if (view.width == 0 || view.height == 0) {
            handler.postDelayed({ globe() }, 200)
            return
        }
        at = SystemClock.uptimeMillis() + 3000
        val w = view.width.toFloat()
        val h = view.height.toFloat()
        val cx = w / 2
        val cy = h / 2
        for (fy in floatArrayOf(0.3f, 0.5f, 0.7f)) {
            drag(w * 0.2f, h * fy, w * 0.8f, h * fy, 900)
            pause(500)
            drag(w * 0.8f, h * fy, w * 0.2f, h * fy, 900)
            pause(500)
        }
        repeat(3) {
            drag(w * 0.3f, cy, w * 0.7f, cy, 120)
            pause(1300)
            drag(w * 0.7f, cy, w * 0.3f, cy, 120)
            pause(1300)
        }
        drag(cx, h * 0.3f, cx, h * 0.8f, 900)
        pause(500)
        drag(cx, h * 0.8f, cx, h * 0.2f, 900)
        pause(500)
        drag(w * 0.2f, h * 0.3f, w * 0.8f, h * 0.7f, 900)
        pause(500)
        repeat(3) {
            pinch(cx, cy, w * 0.15f, w * 0.6f, 500)
            pause(600)
        }
        drag(w * 0.25f, cy, w * 0.75f, cy, 700)
        pause(600)
        drag(w * 0.75f, cy, w * 0.25f, cy, 700)
        pause(600)
        repeat(3) {
            pinch(cx, cy, w * 0.6f, w * 0.15f, 500)
            pause(600)
        }
        tilt(cx, cy + h * 0.15f, w * 0.35f, -h * 0.25f, 700)
        pause(600)
        drag(w * 0.2f, cy, w * 0.8f, cy, 900)
        pause(500)
        drag(w * 0.8f, cy, w * 0.2f, cy, 900)
        pause(500)
        tilt(cx, cy - h * 0.1f, w * 0.35f, h * 0.25f, 700)
        pause(600)
        twist(cx, cy, w * 0.45f, 60.0, 700)
        pause(600)
        drag(w * 0.2f, h * 0.4f, w * 0.8f, h * 0.4f, 900)
        pause(500)
        twist(cx, cy, w * 0.45f, -60.0, 700)
        pause(600)
        drag(w * 0.08f, h * 0.06f, cx, cy, 900)
        pause(800)
        handler.postAtTime(done, at)
    }

    fun views() {
        if (view.width == 0 || view.height == 0) {
            handler.postDelayed({ views() }, 200)
            return
        }
        val map = view as? MapView ?: return
        at = SystemClock.uptimeMillis() + 3000
        val w = view.width.toFloat()
        val h = view.height.toFloat()
        val cx = w / 2
        val cy = h / 2
        repeat(3) {
            pinch(cx, cy, w * 0.15f, w * 0.6f, 500)
            pause(700)
        }
        pause(1500)
        for (next in listOf("terrain", "tilted", "globe", "terrain", "globe")) {
            call { map.setView(next) }
            pause(2500)
        }
        repeat(3) {
            pinch(cx, cy, w * 0.6f, w * 0.15f, 500)
            pause(700)
        }
        pause(1500)
        drag(cx, h * 0.3f, cx, h * 0.62f, 900)
        pause(2000)
        call { map.setView("terrain") }
        pause(2500)
        call { map.setView("globe") }
        pause(2500)
        handler.postAtTime(done, at)
    }

    fun script(json: JSONObject) {
        if (view.width == 0 || view.height == 0) {
            handler.postDelayed({ script(json) }, 200)
            return
        }
        at = SystemClock.uptimeMillis() + 1000
        run(json.getJSONArray("steps"), view.width.toFloat(), view.height.toFloat())
        handler.postAtTime({ Log.i(TAG, "done") }, at)
        handler.postAtTime(done, at)
    }

    private fun run(steps: JSONArray, w: Float, h: Float) {
        for (i in 0 until steps.length()) {
            val s = steps.getJSONObject(i)
            val ms = s.optLong("ms", 300)
            val x = s.optDouble("x", 0.5).toFloat() * w
            val y = s.optDouble("y", 0.5).toFloat() * h
            when (s.getString("op")) {
                "pause" -> pause(ms)
                "mark" -> {
                    val name = s.getString("name")
                    call { Log.i(TAG, "mark $name") }
                }
                "view" -> {
                    val name = s.getString("name")
                    call { (view as? MapView)?.setView(name) }
                }
                "fly" -> {
                    val lng = s.getDouble("lng")
                    val lat = s.getDouble("lat")
                    val zoom = s.getDouble("zoom")
                    val bearing = s.optDouble("bearing", 0.0)
                    val pitch = s.optDouble("pitch", 0.0)
                    call { (view as? MapView)?.flyTo(lng, lat, zoom, bearing, pitch) }
                }
                "pinch" -> pinch(x, y, s.getDouble("from").toFloat() * w, s.getDouble("to").toFloat() * w, ms)
                "twist" -> twist(x, y, s.getDouble("gap").toFloat() * w, s.getDouble("deg"), ms)
                "tilt" -> tilt(x, y, s.getDouble("gap").toFloat() * w, s.getDouble("dy").toFloat() * h, ms)
                "drag" -> drag(s.getDouble("x0").toFloat() * w, s.getDouble("y0").toFloat() * h, s.getDouble("x1").toFloat() * w, s.getDouble("y1").toFloat() * h, ms)
                "repeat" -> repeat(s.getInt("times")) { run(s.getJSONArray("steps"), w, h) }
            }
        }
    }

    private fun call(action: () -> Unit) {
        handler.postAtTime(action, at)
    }

    private fun pause(ms: Long) {
        at += ms
    }

    private fun pinch(cx: Float, cy: Float, from: Float, to: Float, ms: Long) = two(ms) { k ->
        val d = (from + (to - from) * k) / 2
        floatArrayOf(cx - d, cy, cx + d, cy)
    }

    private fun twist(cx: Float, cy: Float, gap: Float, degrees: Double, ms: Long) = two(ms) { k ->
        val a = Math.toRadians(degrees * k)
        val dx = (cos(a) * gap / 2).toFloat()
        val dy = (sin(a) * gap / 2).toFloat()
        floatArrayOf(cx - dx, cy - dy, cx + dx, cy + dy)
    }

    private fun tilt(cx: Float, cy: Float, gap: Float, dy: Float, ms: Long) = two(ms) { k ->
        val y = cy + dy * k
        floatArrayOf(cx - gap / 2, y, cx + gap / 2, y)
    }

    private fun drag(x0: Float, y0: Float, x1: Float, y1: Float, ms: Long) {
        val down = at
        val frames = (ms / 16).toInt().coerceAtLeast(2)
        post(down, down, MotionEvent.ACTION_DOWN, floatArrayOf(x0, y0))
        for (i in 1..frames) {
            val k = i.toFloat() / frames
            post(down, down + ms * i / frames, MotionEvent.ACTION_MOVE, floatArrayOf(x0 + (x1 - x0) * k, y0 + (y1 - y0) * k))
        }
        post(down, down + ms + 16, MotionEvent.ACTION_UP, floatArrayOf(x1, y1))
        at = down + ms + 32
    }

    private fun two(ms: Long, path: (Float) -> FloatArray) {
        val down = at
        val frames = (ms / 16).toInt().coerceAtLeast(2)
        val start = path(0f)
        post(down, down, MotionEvent.ACTION_DOWN, floatArrayOf(start[0], start[1]))
        post(down, down + 8, MotionEvent.ACTION_POINTER_DOWN or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), start)
        for (i in 1..frames) post(down, down + 8 + ms * i / frames, MotionEvent.ACTION_MOVE, path(i.toFloat() / frames))
        val end = path(1f)
        post(down, down + ms + 24, MotionEvent.ACTION_POINTER_UP or (1 shl MotionEvent.ACTION_POINTER_INDEX_SHIFT), end)
        post(down, down + ms + 40, MotionEvent.ACTION_UP, floatArrayOf(end[0], end[1]))
        at = down + ms + 56
    }

    private fun post(down: Long, time: Long, action: Int, xy: FloatArray) {
        handler.postAtTime({
            val count = xy.size / 2
            val props = Array(count) { i -> MotionEvent.PointerProperties().apply { id = i; toolType = MotionEvent.TOOL_TYPE_FINGER } }
            val coords = Array(count) { i -> MotionEvent.PointerCoords().apply { x = xy[i * 2]; y = xy[i * 2 + 1]; pressure = 1f; size = 0.1f } }
            val event = MotionEvent.obtain(down, time, action, count, props, coords, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
            view.dispatchTouchEvent(event)
            event.recycle()
        }, time)
    }
}
