$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Runtime.WindowsRuntime
Add-Type -AssemblyName System.Drawing
$null = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
$null = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties, Windows.Media.Control, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.IRandomAccessStreamWithContentType, Windows.Storage.Streams, ContentType = WindowsRuntime]

if (-not ('MerryAlbumRenderer' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class MerryAlbumRenderer
{
    public const int Width = 202;
    public const int Height = 220;
    private const int ArtTop = 9;
    private const int ArtSize = 202;

    public static byte[] Render(Stream source, bool playing)
    {
        using (Image original = Image.FromStream(source, true, true))
        using (Bitmap canvas = new Bitmap(Width, Height, PixelFormat.Format24bppRgb))
        using (Graphics graphics = Graphics.FromImage(canvas))
        {
            graphics.Clear(Color.Black);
            graphics.CompositingMode = CompositingMode.SourceCopy;
            graphics.CompositingQuality = CompositingQuality.HighQuality;
            graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            graphics.SmoothingMode = SmoothingMode.HighQuality;

            int crop = Math.Min(original.Width, original.Height);
            int sourceX = (original.Width - crop) / 2;
            int sourceY = (original.Height - crop) / 2;
            graphics.DrawImage(original, new Rectangle(0, ArtTop, ArtSize, ArtSize),
                sourceX, sourceY, crop, crop, GraphicsUnit.Pixel);

            graphics.CompositingMode = CompositingMode.SourceOver;
            const int circleSize = 46;
            const int circleX = 148;
            const int circleY = 158;
            using (Brush shadow = new SolidBrush(Color.FromArgb(205, 0, 0, 0)))
            using (Pen edge = new Pen(Color.FromArgb(210, 255, 255, 255), 2.0f))
            using (Brush glyph = new SolidBrush(Color.White))
            {
                graphics.FillEllipse(shadow, circleX, circleY, circleSize, circleSize);
                graphics.DrawEllipse(edge, circleX + 1, circleY + 1, circleSize - 2, circleSize - 2);
                if (playing)
                {
                    graphics.FillRectangle(glyph, circleX + 15, circleY + 13, 6, 20);
                    graphics.FillRectangle(glyph, circleX + 26, circleY + 13, 6, 20);
                }
                else
                {
                    Point[] triangle = {
                        new Point(circleX + 17, circleY + 12),
                        new Point(circleX + 17, circleY + 34),
                        new Point(circleX + 34, circleY + 23)
                    };
                    graphics.FillPolygon(glyph, triangle);
                }
            }

            Rectangle bounds = new Rectangle(0, 0, Width, Height);
            BitmapData data = canvas.LockBits(bounds, ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            try
            {
                byte[] output = new byte[Width * Height * 2];
                byte[] row = new byte[Math.Abs(data.Stride)];
                for (int y = 0; y < Height; y++)
                {
                    IntPtr rowPointer = IntPtr.Add(data.Scan0, y * data.Stride);
                    Marshal.Copy(rowPointer, row, 0, row.Length);
                    for (int x = 0; x < Width; x++)
                    {
                        int input = x * 3;
                        byte blue = row[input];
                        byte green = row[input + 1];
                        byte red = row[input + 2];
                        ushort rgb565 = (ushort)(((red & 0xf8) << 8) |
                            ((green & 0xfc) << 3) | (blue >> 3));
                        int destination = (y * Width + x) * 2;
                        output[destination] = (byte)(rgb565 & 0xff);
                        output[destination + 1] = (byte)(rgb565 >> 8);
                    }
                }
                return output;
            }
            finally
            {
                canvas.UnlockBits(data);
            }
        }
    }
}
'@
}

$script:merryAsTaskMethod = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object {
        $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and
        $_.GetParameters().Count -eq 1
    } |
    Select-Object -First 1
$script:merryAsStreamMethod = [System.IO.WindowsRuntimeStreamExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsStreamForRead' -and $_.GetParameters().Count -eq 1 } |
    Select-Object -First 1
$script:merryMediaManager = $null

function Wait-MerryWinRt {
    param($Operation, [Type]$ResultType)

    $task = $script:merryAsTaskMethod.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    try {
        $task.Wait()
    }
    catch {
        throw $task.Exception.Flatten().InnerException
    }
    return $task.Result
}

function Initialize-MerryMedia {
    $script:merryMediaManager = Wait-MerryWinRt (
        [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()
    ) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])
}

function Get-MerrySpotifySnapshot {
    if ($null -eq $script:merryMediaManager) {
        Initialize-MerryMedia
    }
    $session = @($script:merryMediaManager.GetSessions()) |
        Where-Object SourceAppUserModelId -Match 'Spotify' |
        Select-Object -First 1
    if ($null -eq $session) {
        return [pscustomobject]@{ State = 'None'; Key = $null; Properties = $null }
    }

    $status = [string]$session.GetPlaybackInfo().PlaybackStatus
    if ($status -notin @('Playing', 'Paused')) {
        return [pscustomobject]@{ State = 'None'; Key = $null; Properties = $null }
    }
    $properties = Wait-MerryWinRt ($session.TryGetMediaPropertiesAsync()) (
        [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties]
    )
    if ($null -eq $properties.Thumbnail) {
        return [pscustomobject]@{ State = 'None'; Key = $null; Properties = $null }
    }
    $key = '{0}|{1}|{2}|{3}' -f $properties.Title, $properties.Artist,
        $properties.AlbumTitle, $status
    return [pscustomobject]@{ State = $status; Key = $key; Properties = $properties }
}

function ConvertTo-MerryAlbumBytes {
    param($Snapshot)

    $winRtStream = Wait-MerryWinRt ($Snapshot.Properties.Thumbnail.OpenReadAsync()) (
        [Windows.Storage.Streams.IRandomAccessStreamWithContentType]
    )
    $dotNetStream = $script:merryAsStreamMethod.Invoke($null, @($winRtStream))
    try {
        return ,[MerryAlbumRenderer]::Render($dotNetStream, $Snapshot.State -eq 'Playing')
    }
    finally {
        $dotNetStream.Dispose()
        if ([System.Runtime.InteropServices.Marshal]::IsComObject($winRtStream)) {
            $null = [System.Runtime.InteropServices.Marshal]::ReleaseComObject($winRtStream)
        }
    }
}

