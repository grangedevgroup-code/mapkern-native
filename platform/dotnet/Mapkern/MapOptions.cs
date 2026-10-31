namespace Mapkern;

public sealed record MapOptions
{
    public string Style { get; init; } = "https://tiles.openfreemap.org/styles/liberty";
    public double Longitude { get; init; }
    public double Latitude { get; init; }
    public double Zoom { get; init; }
    public double Bearing { get; init; }
    public double Pitch { get; init; }
    public double MinZoom { get; init; } = -2;
    public double MaxZoom { get; init; } = 22;
    public double MaxPitch { get; init; } = 85;
    public bool Globe { get; init; } = true;
    public string? CacheDirectory { get; init; }
    public bool Offline { get; init; }
    public bool Controls { get; init; }
    public bool TouchControls { get; init; }
    public string View { get; init; } = "globe";
}
