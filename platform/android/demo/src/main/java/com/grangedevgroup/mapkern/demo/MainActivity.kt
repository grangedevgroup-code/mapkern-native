package com.grangedevgroup.mapkern.demo

import android.app.Activity
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.WindowInsets
import android.widget.FrameLayout
import com.grangedevgroup.mapkern.Diagnostics
import com.grangedevgroup.mapkern.DiagnosticsOptions
import com.grangedevgroup.mapkern.MapOptions
import com.grangedevgroup.mapkern.MapView

class MainActivity : Activity() {
    private lateinit var map: MapView
    private lateinit var diagnostics: Diagnostics

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val extras = intent.extras
        fun number(key: String) = extras?.getString(key)?.toDoubleOrNull()
        map = MapView(this, options = MapOptions(longitude = number("lng") ?: 0.0, latitude = number("lat") ?: 0.0, zoom = number("zoom") ?: 0.0, controls = true))
        val frame = FrameLayout(this)
        frame.setBackgroundColor(Color.rgb(17, 25, 31))
        frame.addView(map, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        if (Build.VERSION.SDK_INT >= 30) {
            frame.setOnApplyWindowInsetsListener { view, insets ->
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
                WindowInsets.CONSUMED
            }
        }
        setContentView(frame)
        diagnostics = Diagnostics(this, map, DiagnosticsOptions(hud = extras?.getBoolean("hud", false) == true))
        diagnostics.start()
        extras?.getString("view")?.let { map.setView(it) }
        extras?.getString("basemap")?.toIntOrNull()?.let { map.selectBasemap(it) }
        if (extras?.getString("bench") == "gestures") map.post { Bench(map).gestures() }
    }

    override fun onResume() {
        super.onResume()
        map.onResume()
        diagnostics.setForeground(true)
    }

    override fun onPause() {
        diagnostics.setForeground(false)
        map.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        diagnostics.stop()
        map.destroy()
        super.onDestroy()
    }
}
