# Mapkern Native

Mapkern Native is the C++ version of [Mapkern GL JS](https://github.com/grangedevgroup-code/mapkern-gl-js). It draws the same vector and raster maps, as a flat map, a tilted 3D terrain view or a globe, inside native apps on Android and Fire tablets, iOS, macOS, Windows and Linux. Styles, tile sources and the demo controls work the same way they do in the browser version.

The engine is one C++20 library. It talks to the GPU through sokol_gfx, so it uses Direct3D 11 on Windows, Metal on Apple devices, OpenGL ES 3 on Android and OpenGL on Linux. On top of it sit a C API and thin SDKs: Kotlin for Android, Swift for iOS and macOS, and C# for Windows.

## Build and run

You need CMake 4 and Ninja. On Windows, run `build.cmd` from a normal command prompt; it finds Visual Studio and builds into `build/windows`. Elsewhere:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/apps/viewer/mapkern-viewer --center -122.33 47.61 --zoom 11
```

The viewer opens a window with the same controls as the GL JS demo: the basemap button, the 2D/3D/globe button, zoom, compass and scale. Drag to pan, scroll to zoom, and add `--view tilted` to start in 3D. The tests run from the `test` folder with `../build/test/mapkern_unit`.

## Using it from an app

On Android, the `platform/android` Gradle project builds an AAR and a demo app. Add the AAR and put a `MapView` in your layout:

```kotlin
val map = MapView(context, options = MapOptions(longitude = -122.33, latitude = 47.61, zoom = 11.0, controls = true))
setContentView(map)
```

On iOS and macOS, build the XCFramework with `platform/apple/build-xcframework.sh`, then add the Swift package in `platform/apple`. `MapView` is an `MTKView`, so it goes anywhere a view does:

```swift
let map = MapView(frame: bounds, options: MapOptions(longitude: -122.33, latitude: 47.61, zoom: 11, controls: true))
```

On Windows, `platform/dotnet` has the `Mapkern.Native` library and a WinForms `MapControl`. Build the native library first (`build.cmd`), then:

```csharp
var map = new MapControl(new MapOptions { Longitude = -122.33, Latitude = 47.61, Zoom = 11, Controls = true });
form.Controls.Add(map);
```

Anything else can use the C API in `include/mapkern/mapkern.h`. On Windows, `mk_view_create_hwnd` takes a window handle and sets up the swapchain for you; on other platforms you pass your own Metal or GL surface to `mk_map_render`.

## Styles and data

Mapkern reads its own style format and MapLibre/Mapbox v8 styles, which it converts on load. Tiles come from URLs, TileJSON, PMTiles and MBTiles files, local or over HTTP range requests. Elevation can be Terrarium or Mapbox encoded PNG or WebP tiles. Every response goes through a disk cache, so places you've seen keep working offline, and `downloadRegion` fills the cache for an area ahead of time.

## Headless frames

`mapkern-render` (Windows) draws a map off screen and writes a PNG once every tile has loaded. It's how the screenshots in issues are made:

```sh
mapkern-render --center -121.76 46.85 --zoom 11 --pitch 60 --terrain 1.5 --out rainier.png
```

## License

MIT. See `LICENSE`.
