using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Mapkern.WinForms;

public partial class MapControl : Control
{
    private const int WmPointerDown = 0x0246;
    private const int WmPointerUpdate = 0x0245;
    private const int WmPointerUp = 0x0247;
    private const int PtTouch = 2;
    private const int PtPen = 3;

    private readonly MapOptions options;
    private readonly Timer timer = new() { Interval = 15 };
    private MapView? view;
    private bool mouseDown;

    public MapControl() : this(new MapOptions()) { }

    public MapControl(MapOptions options)
    {
        this.options = options;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.Opaque | ControlStyles.Selectable, true);
        timer.Tick += (_, _) => Frame();
    }

    [Browsable(false)]
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public MapView? Map => view;

    public event EventHandler<MapEventArgs>? MapEvent;

    private double Ratio => DeviceDpi / 96.0;

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        if (DesignMode) return;
        view = MapView.CreateForWindow(Handle, options, Ratio);
        if (view is null) return;
        view.Invalidated += (_, _) =>
        {
            if (IsHandleCreated && !IsDisposed) BeginInvoke(new Action(StartFrames));
        };
        view.Event += (_, args) =>
        {
            if (IsHandleCreated && !IsDisposed) BeginInvoke(new Action(() => MapEvent?.Invoke(this, args)));
        };
        StartFrames();
    }

    protected override void OnHandleDestroyed(EventArgs e)
    {
        timer.Stop();
        view?.Dispose();
        view = null;
        base.OnHandleDestroyed(e);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) timer.Dispose();
        base.Dispose(disposing);
    }

    private void StartFrames()
    {
        if (!timer.Enabled) timer.Start();
    }

    private void Frame()
    {
        if (view is null || ClientSize.Width <= 0 || ClientSize.Height <= 0)
        {
            timer.Stop();
            return;
        }
        if (!view.Render()) timer.Stop();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        view?.Render();
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        if (view is null || ClientSize.Width <= 0 || ClientSize.Height <= 0) return;
        view.Resize(ClientSize.Width, ClientSize.Height, Ratio);
        StartFrames();
    }

    protected override void OnDpiChangedAfterParent(EventArgs e)
    {
        base.OnDpiChangedAfterParent(e);
        OnResize(e);
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        Focus();
        if (e.Button != MouseButtons.Left || view is null) return;
        mouseDown = true;
        Capture = true;
        view.PointerDown(0, e.X / Ratio, e.Y / Ratio);
        StartFrames();
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (!mouseDown || view is null) return;
        view.PointerMove(0, e.X / Ratio, e.Y / Ratio);
        StartFrames();
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (!mouseDown || view is null) return;
        mouseDown = false;
        Capture = false;
        view.PointerUp(0, e.X / Ratio, e.Y / Ratio);
        StartFrames();
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        if (view is null) return;
        view.Wheel(e.Delta / 480.0, e.X / Ratio, e.Y / Ratio);
        StartFrames();
    }

    protected override void WndProc(ref Message m)
    {
        if (view is not null && (m.Msg == WmPointerDown || m.Msg == WmPointerUpdate || m.Msg == WmPointerUp))
        {
            var id = (int)(m.WParam.ToInt64() & 0xffff);
            if (GetPointerType((uint)id, out var type) && (type == PtTouch || type == PtPen))
            {
                var lp = m.LParam.ToInt64();
                var point = PointToClient(new System.Drawing.Point((short)(lp & 0xffff), (short)((lp >> 16) & 0xffff)));
                var x = point.X / Ratio;
                var y = point.Y / Ratio;
                if (m.Msg == WmPointerDown) view.PointerDown(id + 1, x, y);
                else if (m.Msg == WmPointerUpdate) view.PointerMove(id + 1, x, y);
                else view.PointerUp(id + 1, x, y);
                StartFrames();
                m.Result = IntPtr.Zero;
                return;
            }
        }
        base.WndProc(ref m);
    }

    [LibraryImport("user32.dll")]
    [DefaultDllImportSearchPaths(DllImportSearchPath.System32)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetPointerType(uint pointerId, out int pointerType);
}
