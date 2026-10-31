using System;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;
using Mapkern;
using Mapkern.WinForms;

namespace MapkernDemo;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        var smoke = args.Contains("--smoke");
        var form = new Form { Text = "Mapkern", Width = 1280, Height = 800, StartPosition = FormStartPosition.CenterScreen };
        if (smoke)
        {
            form.StartPosition = FormStartPosition.Manual;
            form.Location = new Point(-32000, -32000);
            form.ShowInTaskbar = false;
        }
        var map = new MapControl(new MapOptions { Longitude = -122.33, Latitude = 47.61, Zoom = 2.5, Controls = true }) { Dock = DockStyle.Fill };
        form.Controls.Add(map);
        var loaded = false;
        var errors = 0;
        using var timer = new Timer { Interval = 8000 };
        if (smoke)
        {
            map.MapEvent += (_, e) =>
            {
                if (e.Type == "styleload") loaded = true;
                if (e.Type == "error") errors++;
                Console.WriteLine($"{e.Type} {e.Message}");
            };
            timer.Tick += (_, _) =>
            {
                var camera = map.Map?.Camera;
                Console.WriteLine($"view={(map.Map is null ? "none" : "ok")} loaded={loaded} errors={errors} zoom={camera?.Zoom:0.00} attribution={map.Map?.Attribution}");
                form.Close();
            };
            timer.Start();
        }
        Application.Run(form);
        return smoke && (map.Map is null && !loaded) ? 1 : 0;
    }
}
