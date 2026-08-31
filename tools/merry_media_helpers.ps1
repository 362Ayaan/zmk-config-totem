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

    public static byte[] Render(Stream source)
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

public static class MerryHostActivity
{
    [StructLayout(LayoutKind.Sequential)]
    private struct LASTINPUTINFO
    {
        public uint cbSize;
        public uint dwTime;
    }

    [DllImport("user32.dll")]
    private static extern bool GetLastInputInfo(ref LASTINPUTINFO info);

    public static uint IdleMilliseconds()
    {
        LASTINPUTINFO info = new LASTINPUTINFO();
        info.cbSize = (uint)Marshal.SizeOf(info);
        if (!GetLastInputInfo(ref info))
            throw new InvalidOperationException("GetLastInputInfo failed.");
        return unchecked((uint)Environment.TickCount - info.dwTime);
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
$script:merryManagerRefreshedAt = [DateTime]::MinValue

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
    if ($null -ne $script:merryMediaManager -and
        [System.Runtime.InteropServices.Marshal]::IsComObject($script:merryMediaManager)) {
        try {
            $null = [System.Runtime.InteropServices.Marshal]::ReleaseComObject(
                $script:merryMediaManager
            )
        } catch {}
    }
    $script:merryMediaManager = Wait-MerryWinRt (
        [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()
    ) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])
    $script:merryManagerRefreshedAt = [DateTime]::UtcNow
}

function Get-MerrySpotifyPlayback {
    if ($null -eq $script:merryMediaManager) {
        Initialize-MerryMedia
    }

    $candidates = @()
    foreach ($session in @($script:merryMediaManager.GetSessions()) |
        Where-Object SourceAppUserModelId -Match 'Spotify') {
        try {
            $status = [string]$session.GetPlaybackInfo().PlaybackStatus
            $rank = switch ($status) {
                'Playing' { 0 }
                'Paused' { 1 }
                default { 2 }
            }
            $candidates += [pscustomobject]@{
                Session = $session
                State = $status
                Rank = $rank
            }
        }
        catch {
            # A dead session can remain in GetSessions briefly. Ignore it and
            # prefer a live Spotify session from the same manager snapshot.
        }
    }

    $selected = $candidates | Sort-Object Rank | Select-Object -First 1
    if (($null -eq $selected -or $selected.State -notin @('Playing', 'Paused')) -and
        [DateTime]::UtcNow -ge $script:merryManagerRefreshedAt.AddSeconds(10)) {
        Initialize-MerryMedia
        foreach ($session in @($script:merryMediaManager.GetSessions()) |
            Where-Object SourceAppUserModelId -Match 'Spotify') {
            try {
                $status = [string]$session.GetPlaybackInfo().PlaybackStatus
                if ($status -in @('Playing', 'Paused')) {
                    return [pscustomobject]@{ State = $status; Session = $session }
                }
            }
            catch {}
        }
    }

    if ($null -eq $selected -or $selected.State -notin @('Playing', 'Paused')) {
        return [pscustomobject]@{ State = 'None'; Session = $null }
    }
    return [pscustomobject]@{ State = $selected.State; Session = $selected.Session }
}

function Get-MerrySpotifyTrack {
    param($Playback)

    if ($null -eq $Playback.Session -or $Playback.State -notin @('Playing', 'Paused')) {
        return [pscustomobject]@{
            State = 'None'; Key = $null; Properties = $null; Session = $null
        }
    }
    $properties = Wait-MerryWinRt ($Playback.Session.TryGetMediaPropertiesAsync()) (
        [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties]
    )
    if ($null -eq $properties.Thumbnail) {
        return [pscustomobject]@{
            State = $Playback.State; Key = $null; Properties = $null
            Session = $Playback.Session
        }
    }
    $key = '{0}|{1}|{2}' -f $properties.Title, $properties.Artist,
        $properties.AlbumTitle
    return [pscustomobject]@{
        State = $Playback.State; Key = $key; Properties = $properties
        Session = $Playback.Session
    }
}

function Get-MerrySpotifySnapshot {
    return Get-MerrySpotifyTrack -Playback (Get-MerrySpotifyPlayback)
}

function ConvertTo-MerryAlbumBytes {
    param($Snapshot)

    $winRtStream = Wait-MerryWinRt ($Snapshot.Properties.Thumbnail.OpenReadAsync()) (
        [Windows.Storage.Streams.IRandomAccessStreamWithContentType]
    )
    $dotNetStream = $script:merryAsStreamMethod.Invoke($null, @($winRtStream))
    try {
        return ,[MerryAlbumRenderer]::Render($dotNetStream)
    }
    finally {
        $dotNetStream.Dispose()
        if ([System.Runtime.InteropServices.Marshal]::IsComObject($winRtStream)) {
            $null = [System.Runtime.InteropServices.Marshal]::ReleaseComObject($winRtStream)
        }
    }
}
