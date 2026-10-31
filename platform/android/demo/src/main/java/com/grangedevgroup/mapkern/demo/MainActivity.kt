package com.grangedevgroup.mapkern.demo

import android.app.Activity
import android.os.Bundle
import com.grangedevgroup.mapkern.MapOptions
import com.grangedevgroup.mapkern.MapView

class MainActivity : Activity() {
    private lateinit var map: MapView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        map = MapView(this, options = MapOptions())
        setContentView(map)
    }

    override fun onResume() {
        super.onResume()
        map.onResume()
    }

    override fun onPause() {
        map.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        map.destroy()
        super.onDestroy()
    }
}
