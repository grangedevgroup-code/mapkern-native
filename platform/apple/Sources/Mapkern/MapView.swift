import CMapkern
import Foundation
import MetalKit

#if os(iOS)
import UIKit
#else
import AppKit
#endif

public struct MapOptions {
    public var style = "https://tiles.openfreemap.org/styles/liberty"
    public var longitude = 0.0
    public var latitude = 0.0
    public var zoom = 0.0
    public var bearing = 0.0
    public var pitch = 0.0
    public var globe = true
    public var offline = false
    public var controls = false
    public var view = "globe"
    public var cacheDirectory: String?

    public init(style: String = "https://tiles.openfreemap.org/styles/liberty", longitude: Double = 0, latitude: Double = 0, zoom: Double = 0,
                controls: Bool = false) {
        self.style = style
        self.longitude = longitude
        self.latitude = latitude
        self.zoom = zoom
        self.controls = controls
    }
}

public struct Camera {
    public var longitude: Double
    public var latitude: Double
    public var zoom: Double
    public var bearing: Double
    public var pitch: Double
}

private final class Callbacks {
    weak var view: MapView?
}

public final class MapView: MTKView, MTKViewDelegate {
    private var map: OpaquePointer?
    private let callbacks = Callbacks()
    private var nextPointer: Int32 = 1
    #if os(iOS)
    private var touchIds: [ObjectIdentifier: Int32] = [:]
    #endif
    public var onEvent: ((String, String) -> Void)?

    public init(frame: CGRect, options: MapOptions = MapOptions()) {
        let device = MTLCreateSystemDefaultDevice()
        super.init(frame: frame, device: device)
        colorPixelFormat = .bgra8Unorm
        depthStencilPixelFormat = .depth32Float_stencil8
        sampleCount = 1
        enableSetNeedsDisplay = false
        isPaused = true
        delegate = self
        #if os(iOS)
        isMultipleTouchEnabled = true
        #endif
        callbacks.view = self
        create(options)
    }

    required init(coder: NSCoder) {
        super.init(coder: coder)
        device = MTLCreateSystemDefaultDevice()
        colorPixelFormat = .bgra8Unorm
        depthStencilPixelFormat = .depth32Float_stencil8
        enableSetNeedsDisplay = false
        isPaused = true
        delegate = self
        callbacks.view = self
        create(MapOptions())
    }

    deinit {
        if let map {
            mk_map_set_invalidate(map, nil, nil)
            mk_map_set_events(map, nil, nil)
            mk_map_destroy(map)
        }
    }

    private var scale: Double {
        #if os(iOS)
        return Double(contentScaleFactor)
        #else
        return Double(window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2)
        #endif
    }

    private func create(_ options: MapOptions) {
        let cache = options.cacheDirectory ?? FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent("mapkern").path
        var o = mk_options()
        mk_options_default(&o)
        let style = strdup(options.style)
        let dir = strdup(cache)
        let viewName = strdup(options.view)
        defer {
            free(style)
            free(dir)
            free(viewName)
        }
        o.style = UnsafePointer(style)
        o.cache_directory = UnsafePointer(dir)
        o.view = UnsafePointer(viewName)
        o.lng = options.longitude
        o.lat = options.latitude
        o.zoom = options.zoom
        o.bearing = options.bearing
        o.pitch = options.pitch
        o.globe = options.globe ? 1 : 0
        o.offline = options.offline ? 1 : 0
        o.controls = options.controls ? 1 : 0
        #if os(iOS)
        o.touch_controls = 1
        #else
        o.touch_controls = 0
        #endif
        o.pixel_ratio = scale
        guard let m = mk_map_create(&o) else { return }
        map = m
        let user = Unmanaged.passUnretained(callbacks).toOpaque()
        mk_map_set_invalidate(m, { user in
            guard let user else { return }
            let callbacks = Unmanaged<Callbacks>.fromOpaque(user).takeUnretainedValue()
            DispatchQueue.main.async { callbacks.view?.wake() }
        }, user)
        mk_map_set_events(m, { user, type, message, _ in
            guard let user else { return }
            let callbacks = Unmanaged<Callbacks>.fromOpaque(user).takeUnretainedValue()
            let t = type.map { String(cString: $0) } ?? ""
            let msg = message.map { String(cString: $0) } ?? ""
            DispatchQueue.main.async { callbacks.view?.onEvent?(t, msg) }
        }, user)
        if let device {
            var g = mk_graphics()
            g.metal_device = UnsafeRawPointer(Unmanaged.passUnretained(device).toOpaque())
            g.color_format = Int32(MK_FORMAT_BGRA8)
            g.depth_format = Int32(MK_FORMAT_DEPTH_STENCIL)
            g.sample_count = 1
            mk_map_init_graphics(m, &g)
        }
        wake()
    }

    func wake() {
        isPaused = false
    }

    public func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        guard let map else { return }
        let s = scale
        mk_map_resize(map, Double(size.width) / s, Double(size.height) / s, s)
        wake()
    }

    public func draw(in view: MTKView) {
        guard let map, let drawable = currentDrawable, let depth = depthStencilTexture else { return }
        var surface = mk_surface()
        surface.width = Int32(drawableSize.width)
        surface.height = Int32(drawableSize.height)
        surface.sample_count = 1
        surface.color_format = Int32(MK_FORMAT_BGRA8)
        surface.depth_format = Int32(MK_FORMAT_DEPTH_STENCIL)
        surface.metal_current_drawable = UnsafeRawPointer(Unmanaged.passUnretained(drawable).toOpaque())
        surface.metal_depth_stencil_texture = UnsafeRawPointer(Unmanaged.passUnretained(depth).toOpaque())
        let more = mk_map_render(map, &surface)
        if more == 0 { isPaused = true }
    }

    public func setStyle(_ style: String) {
        guard let map else { return }
        mk_map_set_style(map, style)
        wake()
    }

    public func addSource(_ id: String, json: String) {
        guard let map else { return }
        mk_map_add_source(map, id, json)
        wake()
    }

    public func addLayer(_ json: String, before: String? = nil) {
        guard let map else { return }
        mk_map_add_layer(map, json, before)
        wake()
    }

    public var camera: Camera {
        get {
            var c = mk_camera()
            if let map { mk_map_get_camera(map, &c) }
            return Camera(longitude: c.lng, latitude: c.lat, zoom: c.zoom, bearing: c.bearing, pitch: c.pitch)
        }
        set {
            guard let map else { return }
            var c = mk_camera(lng: newValue.longitude, lat: newValue.latitude, zoom: newValue.zoom, bearing: newValue.bearing, pitch: newValue.pitch)
            mk_map_jump_to(map, &c)
            wake()
        }
    }

    public func fly(to camera: Camera, duration: Double = -1) {
        guard let map else { return }
        var c = mk_camera(lng: camera.longitude, lat: camera.latitude, zoom: camera.zoom, bearing: camera.bearing, pitch: camera.pitch)
        mk_map_fly_to(map, &c, duration)
        wake()
    }

    public func setGlobe(_ globe: Bool) {
        guard let map else { return }
        mk_map_set_projection(map, globe ? 1 : 0)
        wake()
    }

    public func setTerrain(source: String, exaggeration: Double = 1, relief: Double = 0.5) {
        guard let map else { return }
        mk_map_set_terrain(map, source, exaggeration, relief)
        wake()
    }

    public func setOffline(_ offline: Bool) {
        guard let map else { return }
        mk_map_set_offline(map, offline ? 1 : 0)
        wake()
    }

    #if os(iOS)
    private func send(_ touches: Set<UITouch>, _ action: Int) {
        guard let map else { return }
        for touch in touches {
            let key = ObjectIdentifier(touch)
            let p = touch.location(in: self)
            if action == 0 {
                let id = nextPointer
                nextPointer += 1
                touchIds[key] = id
                mk_map_pointer_down(map, id, p.x, p.y)
            } else if let id = touchIds[key] {
                if action == 1 {
                    mk_map_pointer_move(map, id, p.x, p.y)
                } else if action == 2 {
                    mk_map_pointer_up(map, id, p.x, p.y)
                    touchIds[key] = nil
                } else {
                    mk_map_pointer_cancel(map, id)
                    touchIds[key] = nil
                }
            }
        }
        wake()
    }

    public override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, 0) }
    public override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, 1) }
    public override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, 2) }
    public override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, 3) }
    #else
    public override var acceptsFirstResponder: Bool { true }

    private func point(_ event: NSEvent) -> CGPoint {
        let p = convert(event.locationInWindow, from: nil)
        return CGPoint(x: p.x, y: bounds.height - p.y)
    }

    public override func mouseDown(with event: NSEvent) {
        guard let map else { return }
        let p = point(event)
        if event.clickCount == 2 {
            mk_map_double_tap(map, p.x, p.y)
        } else {
            mk_map_pointer_down(map, 0, p.x, p.y)
        }
        wake()
    }

    public override func mouseDragged(with event: NSEvent) {
        guard let map else { return }
        let p = point(event)
        mk_map_pointer_move(map, 0, p.x, p.y)
        wake()
    }

    public override func mouseUp(with event: NSEvent) {
        guard let map else { return }
        let p = point(event)
        mk_map_pointer_up(map, 0, p.x, p.y)
        wake()
    }

    public override func scrollWheel(with event: NSEvent) {
        guard let map else { return }
        let p = point(event)
        let delta = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY / 100 : event.scrollingDeltaY / 4
        mk_map_wheel(map, delta, p.x, p.y)
        wake()
    }

    public override func magnify(with event: NSEvent) {
        guard let map else { return }
        let p = point(event)
        mk_map_zoom_by(map, log2(1 + event.magnification), p.x, p.y, 0)
        wake()
    }

    public override func rotate(with event: NSEvent) {
        guard let map else { return }
        mk_map_rotate_by(map, -Double(event.rotation))
        wake()
    }
    #endif
}
