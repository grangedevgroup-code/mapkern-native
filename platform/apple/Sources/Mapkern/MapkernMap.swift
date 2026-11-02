import SwiftUI

#if os(iOS)
public struct MapkernMap: UIViewRepresentable {
    private let options: MapOptions
    private let ready: ((MapView) -> Void)?

    public init(options: MapOptions = MapOptions(), ready: ((MapView) -> Void)? = nil) {
        self.options = options
        self.ready = ready
    }

    public func makeUIView(context: Context) -> MapView {
        let view = MapView(frame: .zero, options: options)
        ready?(view)
        return view
    }

    public func updateUIView(_ view: MapView, context: Context) {}
}
#else
public struct MapkernMap: NSViewRepresentable {
    private let options: MapOptions
    private let ready: ((MapView) -> Void)?

    public init(options: MapOptions = MapOptions(), ready: ((MapView) -> Void)? = nil) {
        self.options = options
        self.ready = ready
    }

    public func makeNSView(context: Context) -> MapView {
        let view = MapView(frame: .zero, options: options)
        ready?(view)
        return view
    }

    public func updateNSView(_ view: MapView, context: Context) {}
}
#endif
