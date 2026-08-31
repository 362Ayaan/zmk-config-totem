$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Runtime.WindowsRuntime
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$null = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
$null = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties, Windows.Media.Control, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.IRandomAccessStreamWithContentType, Windows.Storage.Streams, ContentType = WindowsRuntime]

if (-not ('MerryAlbumRenderer' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing,System.Windows.Forms -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

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

public sealed class MerryPowerMonitor : NativeWindow, IDisposable
{
    private const int WM_CLOSE = 0x0010;
    private const int WM_POWERBROADCAST = 0x0218;
    private const int WM_WTSSESSION_CHANGE = 0x02B1;
    private const int PBT_APMSUSPEND = 0x0004;
    private const int PBT_APMRESUMEAUTOMATIC = 0x0012;
    private const int PBT_POWERSETTINGCHANGE = 0x8013;
    private const int WTS_SESSION_LOCK = 0x7;
    private const int WTS_SESSION_UNLOCK = 0x8;
    private const int NOTIFY_FOR_THIS_SESSION = 0;
    private const int DEVICE_NOTIFY_WINDOW_HANDLE = 0;
    private static readonly Guid ConsoleDisplayState =
        new Guid("6fe69556-704a-47a0-8f24-c28d936fda47");

    [StructLayout(LayoutKind.Sequential)]
    private struct PowerBroadcastSetting
    {
        public Guid PowerSetting;
        public uint DataLength;
    }

    [DllImport("wtsapi32.dll", SetLastError = true)]
    private static extern bool WTSRegisterSessionNotification(IntPtr window, int flags);

    [DllImport("wtsapi32.dll")]
    private static extern bool WTSUnRegisterSessionNotification(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr RegisterPowerSettingNotification(
        IntPtr recipient, ref Guid setting, int flags);

    [DllImport("user32.dll")]
    private static extern bool UnregisterPowerSettingNotification(IntPtr handle);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(IntPtr window, uint message,
        IntPtr wParam, IntPtr lParam);

    private readonly ManualResetEvent ready = new ManualResetEvent(false);
    private Thread thread;
    private IntPtr powerNotification;
    private int locked;
    private int displayOff;
    private int revision;
    private string failure;

    public bool Blanked
    {
        get { return Volatile.Read(ref locked) != 0 ||
                     Volatile.Read(ref displayOff) != 0; }
    }

    public bool Locked { get { return Volatile.Read(ref locked) != 0; } }
    public bool DisplayOff { get { return Volatile.Read(ref displayOff) != 0; } }
    public int Revision { get { return Volatile.Read(ref revision); } }
    public string Failure { get { return failure; } }

    public void Start()
    {
        if (thread != null)
            return;
        thread = new Thread(MessageLoop);
        thread.IsBackground = true;
        thread.Name = "Merry Windows power monitor";
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        if (!ready.WaitOne(TimeSpan.FromSeconds(5)))
            throw new TimeoutException("Windows power monitor did not initialize.");
        if (!String.IsNullOrEmpty(failure))
            throw new InvalidOperationException(failure);
    }

    private void MessageLoop()
    {
        try
        {
            CreateParams parameters = new CreateParams();
            parameters.Caption = "MerryPowerMonitor";
            CreateHandle(parameters);
            if (!WTSRegisterSessionNotification(Handle, NOTIFY_FOR_THIS_SESSION))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            Guid setting = ConsoleDisplayState;
            powerNotification = RegisterPowerSettingNotification(
                Handle, ref setting, DEVICE_NOTIFY_WINDOW_HANDLE);
            if (powerNotification == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            ready.Set();
            Application.Run();
        }
        catch (Exception exception)
        {
            failure = exception.Message;
            ready.Set();
        }
        finally
        {
            CleanupHandles();
        }
    }

    private void SetFlag(ref int target, bool value)
    {
        int next = value ? 1 : 0;
        if (Interlocked.Exchange(ref target, next) != next)
            Interlocked.Increment(ref revision);
    }

    protected override void WndProc(ref Message message)
    {
        if (message.Msg == WM_WTSSESSION_CHANGE)
        {
            int reason = message.WParam.ToInt32();
            if (reason == WTS_SESSION_LOCK)
                SetFlag(ref locked, true);
            else if (reason == WTS_SESSION_UNLOCK)
                SetFlag(ref locked, false);
        }
        else if (message.Msg == WM_POWERBROADCAST)
        {
            int reason = message.WParam.ToInt32();
            if (reason == PBT_APMSUSPEND)
            {
                SetFlag(ref displayOff, true);
            }
            else if (reason == PBT_APMRESUMEAUTOMATIC)
            {
                SetFlag(ref displayOff, false);
            }
            else if (reason == PBT_POWERSETTINGCHANGE && message.LParam != IntPtr.Zero)
            {
                PowerBroadcastSetting setting = (PowerBroadcastSetting)Marshal.PtrToStructure(
                    message.LParam, typeof(PowerBroadcastSetting));
                if (setting.PowerSetting == ConsoleDisplayState && setting.DataLength >= 4)
                {
                    int state = Marshal.ReadInt32(message.LParam, 20);
                    SetFlag(ref displayOff, state == 0);
                }
            }
        }
        else if (message.Msg == WM_CLOSE)
        {
            CleanupHandles();
            DestroyHandle();
            Application.ExitThread();
            return;
        }
        base.WndProc(ref message);
    }

    private void CleanupHandles()
    {
        if (powerNotification != IntPtr.Zero)
        {
            UnregisterPowerSettingNotification(powerNotification);
            powerNotification = IntPtr.Zero;
        }
        if (Handle != IntPtr.Zero)
            WTSUnRegisterSessionNotification(Handle);
    }

    public void Dispose()
    {
        IntPtr window = Handle;
        if (window != IntPtr.Zero)
            PostMessage(window, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
        if (thread != null && thread.IsAlive && Thread.CurrentThread != thread)
            thread.Join(TimeSpan.FromSeconds(2));
        ready.Dispose();
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
