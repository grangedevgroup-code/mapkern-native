using System;
using System.Runtime.InteropServices;

namespace Mapkern;

[StructLayout(LayoutKind.Sequential)]
internal struct NativeOptions
{
    public IntPtr Style;
    public double Lng, Lat, Zoom, Bearing, Pitch;
    public double MinZoom, MaxZoom, MaxPitch;
    public int Globe;
    public double PixelRatio;
    public IntPtr CacheDirectory;
    public ulong DiskCacheBytes;
    public int Offline;
    public int Controls;
    public int TouchControls;
    public IntPtr View;
}

[StructLayout(LayoutKind.Sequential)]
public struct Camera
{
    public double Longitude;
    public double Latitude;
    public double Zoom;
    public double Bearing;
    public double Pitch;
}

internal static partial class Native
{
    private const string Lib = "mapkern";

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void InvalidateFn(IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void EventFn(IntPtr user, IntPtr type, IntPtr message, IntPtr sourceId);

    [LibraryImport(Lib, EntryPoint = "mk_options_default")]
    public static partial void OptionsDefault(ref NativeOptions options);

    [LibraryImport(Lib, EntryPoint = "mk_view_create_hwnd")]
    public static partial IntPtr ViewCreateHwnd(IntPtr hwnd, ref NativeOptions options);

    [LibraryImport(Lib, EntryPoint = "mk_view_map")]
    public static partial IntPtr ViewMap(IntPtr view);

    [LibraryImport(Lib, EntryPoint = "mk_view_resize")]
    public static partial void ViewResize(IntPtr view, int width, int height, double ratio);

    [LibraryImport(Lib, EntryPoint = "mk_view_render")]
    public static partial int ViewRender(IntPtr view);

    [LibraryImport(Lib, EntryPoint = "mk_view_destroy")]
    public static partial void ViewDestroy(IntPtr view);

    [LibraryImport(Lib, EntryPoint = "mk_map_set_invalidate")]
    public static partial void SetInvalidate(IntPtr map, IntPtr fn, IntPtr user);

    [LibraryImport(Lib, EntryPoint = "mk_map_set_events")]
    public static partial void SetEvents(IntPtr map, IntPtr fn, IntPtr user);

    [LibraryImport(Lib, EntryPoint = "mk_map_needs_render")]
    public static partial int NeedsRender(IntPtr map);

    [LibraryImport(Lib, EntryPoint = "mk_map_set_style", StringMarshalling = StringMarshalling.Utf8)]
    public static partial void SetStyle(IntPtr map, string style);

    [LibraryImport(Lib, EntryPoint = "mk_map_add_source", StringMarshalling = StringMarshalling.Utf8)]
    public static partial void AddSource(IntPtr map, string id, string spec);

    [LibraryImport(Lib, EntryPoint = "mk_map_remove_source", StringMarshalling = StringMarshalling.Utf8)]
    public static partial void RemoveSource(IntPtr map, string id);

    [LibraryImport(Lib, EntryPoint = "mk_map_add_layer", StringMarshalling = StringMarshalling.Utf8)]
    public static partial void AddLayer(IntPtr map, string spec, string? beforeId);

    [LibraryImport(Lib, EntryPoint = "mk_map_remove_layer", StringMarshalling = StringMarshalling.Utf8)]
    public static partial void RemoveLayer(IntPtr map, string id);

    [LibraryImport(Lib, EntryPoint = "mk_map_set_paint_property", StringMarshalling = StringMarshalling.Utf8)]
    public static partial void SetPaintProperty(IntPtr map, string layer, string name, string value);

    [LibraryImport(Lib, EntryPoint = "mk_map_set_layer_visible", StringMarshalling = StringMarshalling.Utf8)]
    public static partial void SetLayerVisible(IntPtr map, string layer, int visible);

    [LibraryImport(Lib, EntryPoint = "mk_map_attribution")]
    public static partial nuint Attribution(IntPtr map, IntPtr buffer, nuint size);

    [LibraryImport(Lib, EntryPoint = "mk_map_get_camera")]
    public static partial void GetCamera(IntPtr map, out Camera camera);

    [LibraryImport(Lib, EntryPoint = "mk_map_jump_to")]
    public static partial void JumpTo(IntPtr map, in Camera camera);

    [LibraryImport(Lib, EntryPoint = "mk_map_ease_to")]
    public static partial void EaseTo(IntPtr map, in Camera camera, double durationMs);

    [LibraryImport(Lib, EntryPoint = "mk_map_fly_to")]
    public static partial void FlyTo(IntPtr map, in Camera camera, double durationMs);

    [LibraryImport(Lib, EntryPoint = "mk_map_set_projection")]
    public static partial void SetProjection(IntPtr map, int globe);

    [LibraryImport(Lib, EntryPoint = "mk_map_set_terrain", StringMarshalling = StringMarshalling.Utf8)]
    public static partial void SetTerrain(IntPtr map, string sourceId, double exaggeration, double relief);

    [LibraryImport(Lib, EntryPoint = "mk_map_clear_terrain")]
    public static partial void ClearTerrain(IntPtr map);

    [LibraryImport(Lib, EntryPoint = "mk_map_elevation_at")]
    public static partial int ElevationAt(IntPtr map, double lng, double lat, out double meters);

    [LibraryImport(Lib, EntryPoint = "mk_map_unproject")]
    public static partial int Unproject(IntPtr map, double x, double y, out double lng, out double lat);

    [LibraryImport(Lib, EntryPoint = "mk_map_pointer_down")]
    public static partial void PointerDown(IntPtr map, int id, double x, double y);

    [LibraryImport(Lib, EntryPoint = "mk_map_pointer_move")]
    public static partial void PointerMove(IntPtr map, int id, double x, double y);

    [LibraryImport(Lib, EntryPoint = "mk_map_pointer_up")]
    public static partial void PointerUp(IntPtr map, int id, double x, double y);

    [LibraryImport(Lib, EntryPoint = "mk_map_wheel")]
    public static partial void Wheel(IntPtr map, double delta, double x, double y);

    [LibraryImport(Lib, EntryPoint = "mk_map_double_tap")]
    public static partial void DoubleTap(IntPtr map, double x, double y);

    [LibraryImport(Lib, EntryPoint = "mk_map_rotate_by")]
    public static partial void RotateBy(IntPtr map, double degrees);

    [LibraryImport(Lib, EntryPoint = "mk_map_pitch_by")]
    public static partial void PitchBy(IntPtr map, double degrees);

    [LibraryImport(Lib, EntryPoint = "mk_map_set_offline")]
    public static partial void SetOffline(IntPtr map, int offline);

    [LibraryImport(Lib, EntryPoint = "mk_map_clear_disk_cache")]
    public static partial void ClearDiskCache(IntPtr map);

    [LibraryImport(Lib, EntryPoint = "mk_set_network_reachable")]
    public static partial void SetNetworkReachable(int reachable);
}
