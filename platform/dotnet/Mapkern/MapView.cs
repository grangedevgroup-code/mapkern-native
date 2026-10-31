using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Mapkern;

public sealed class MapEventArgs : EventArgs
{
    public MapEventArgs(string type, string message, string sourceId)
    {
        Type = type;
        Message = message;
        SourceId = sourceId;
    }

    public string Type { get; }
    public string Message { get; }
    public string SourceId { get; }
}

public sealed class MapView : IDisposable
{
    private IntPtr view;
    private readonly IntPtr map;
    private readonly Native.InvalidateFn invalidateFn;
    private readonly Native.EventFn eventFn;

    private MapView(IntPtr view)
    {
        this.view = view;
        map = Native.ViewMap(view);
        invalidateFn = _ => Invalidated?.Invoke(this, EventArgs.Empty);
        eventFn = (_, type, message, source) =>
            Event?.Invoke(this, new MapEventArgs(Utf8(type), Utf8(message), Utf8(source)));
        Native.SetInvalidate(map, Marshal.GetFunctionPointerForDelegate(invalidateFn), IntPtr.Zero);
        Native.SetEvents(map, Marshal.GetFunctionPointerForDelegate(eventFn), IntPtr.Zero);
    }

    public event EventHandler? Invalidated;
    public event EventHandler<MapEventArgs>? Event;

    public static MapView? CreateForWindow(IntPtr hwnd, MapOptions options, double pixelRatio)
    {
        var strings = new[] { options.Style, options.CacheDirectory ?? "", options.View };
        var pointers = new IntPtr[strings.Length];
        try
        {
            for (var i = 0; i < strings.Length; i++) pointers[i] = Marshal.StringToCoTaskMemUTF8(strings[i]);
            var native = new NativeOptions();
            Native.OptionsDefault(ref native);
            native.Style = pointers[0];
            native.Lng = options.Longitude;
            native.Lat = options.Latitude;
            native.Zoom = options.Zoom;
            native.Bearing = options.Bearing;
            native.Pitch = options.Pitch;
            native.MinZoom = options.MinZoom;
            native.MaxZoom = options.MaxZoom;
            native.MaxPitch = options.MaxPitch;
            native.Globe = options.Globe ? 1 : 0;
            native.PixelRatio = pixelRatio;
            native.CacheDirectory = options.CacheDirectory is null ? IntPtr.Zero : pointers[1];
            native.Offline = options.Offline ? 1 : 0;
            native.Controls = options.Controls ? 1 : 0;
            native.TouchControls = options.TouchControls ? 1 : 0;
            native.View = pointers[2];
            var handle = Native.ViewCreateHwnd(hwnd, ref native);
            return handle == IntPtr.Zero ? null : new MapView(handle);
        }
        finally
        {
            foreach (var p in pointers) Marshal.FreeCoTaskMem(p);
        }
    }

    public bool NeedsRender => map != IntPtr.Zero && Native.NeedsRender(map) != 0;

    public bool Render() => view != IntPtr.Zero && Native.ViewRender(view) != 0;

    public void Resize(int pixelWidth, int pixelHeight, double pixelRatio) => Native.ViewResize(view, pixelWidth, pixelHeight, pixelRatio);

    public void SetStyle(string urlOrJson) => Native.SetStyle(map, urlOrJson);
    public void AddSource(string id, string specJson) => Native.AddSource(map, id, specJson);
    public void RemoveSource(string id) => Native.RemoveSource(map, id);
    public void AddLayer(string specJson, string? beforeId = null) => Native.AddLayer(map, specJson, beforeId);
    public void RemoveLayer(string id) => Native.RemoveLayer(map, id);
    public void SetPaintProperty(string layerId, string name, string valueJson) => Native.SetPaintProperty(map, layerId, name, valueJson);
    public void SetLayerVisible(string layerId, bool visible) => Native.SetLayerVisible(map, layerId, visible ? 1 : 0);

    public string Attribution
    {
        get
        {
            var size = (int)Native.Attribution(map, IntPtr.Zero, 0);
            var buffer = Marshal.AllocHGlobal(size + 1);
            try
            {
                Native.Attribution(map, buffer, (nuint)(size + 1));
                return Utf8(buffer);
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }
    }

    public Camera Camera
    {
        get
        {
            Native.GetCamera(map, out var c);
            return c;
        }
        set => Native.JumpTo(map, value);
    }

    public void EaseTo(Camera camera, double durationMs = -1) => Native.EaseTo(map, camera, durationMs);
    public void FlyTo(Camera camera, double durationMs = -1) => Native.FlyTo(map, camera, durationMs);
    public void SetGlobe(bool globe) => Native.SetProjection(map, globe ? 1 : 0);
    public void SetTerrain(string sourceId, double exaggeration = 1, double relief = 0.5) => Native.SetTerrain(map, sourceId, exaggeration, relief);
    public void ClearTerrain() => Native.ClearTerrain(map);
    public double? ElevationAt(double lng, double lat) => Native.ElevationAt(map, lng, lat, out var m) != 0 ? m : null;
    public (double Longitude, double Latitude)? Unproject(double x, double y) =>
        Native.Unproject(map, x, y, out var lng, out var lat) != 0 ? (lng, lat) : null;

    public void PointerDown(int id, double x, double y) => Native.PointerDown(map, id, x, y);
    public void PointerMove(int id, double x, double y) => Native.PointerMove(map, id, x, y);
    public void PointerUp(int id, double x, double y) => Native.PointerUp(map, id, x, y);
    public void Wheel(double delta, double x, double y) => Native.Wheel(map, delta, x, y);
    public void DoubleTap(double x, double y) => Native.DoubleTap(map, x, y);
    public void RotateBy(double degrees) => Native.RotateBy(map, degrees);
    public void PitchBy(double degrees) => Native.PitchBy(map, degrees);
    public void SetOffline(bool offline) => Native.SetOffline(map, offline ? 1 : 0);
    public void ClearDiskCache() => Native.ClearDiskCache(map);
    public static void SetNetworkReachable(bool reachable) => Native.SetNetworkReachable(reachable ? 1 : 0);

    public void Dispose()
    {
        if (view == IntPtr.Zero) return;
        Native.SetInvalidate(map, IntPtr.Zero, IntPtr.Zero);
        Native.SetEvents(map, IntPtr.Zero, IntPtr.Zero);
        Native.ViewDestroy(view);
        view = IntPtr.Zero;
        GC.KeepAlive(invalidateFn);
        GC.KeepAlive(eventFn);
    }

    private static string Utf8(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p) ?? "";
}
