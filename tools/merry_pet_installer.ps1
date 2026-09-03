[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Port,
    [switch]$ConvertOnly,
    [string]$OutputPack
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Drawing

if (-not ('MerryPetPackBuilder' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class MerryPetCrc32
{
    public static uint Compute(byte[] data) { return Compute(data, 0, data.Length); }
    public static uint Compute(byte[] data, int offset, int count)
    {
        uint crc = 0xffffffffu;
        for (int i = 0; i < count; ++i) {
            crc ^= data[offset + i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320u : 0u);
        }
        return crc ^ 0xffffffffu;
    }
}

public static class MerryPetPackBuilder
{
    const int CellWidth = 192, CellHeight = 208, Width = 216, Height = 234;
    static readonly int[] Rows = { 0, 7, 6, 3, 5 };
    static readonly int[] Counts = { 6, 6, 6, 4, 8 };
    static readonly bool[] Loops = { true, true, true, false, false };
    static readonly int[][] Durations = {
        new[] {280,110,110,140,140,320}, new[] {120,120,120,120,120,220},
        new[] {150,150,150,150,150,260}, new[] {140,140,140,280},
        new[] {140,140,140,140,140,140,140,240}
    };

    static void RgbToHsv(double r, double g, double b, out double h, out double s, out double v)
    {
        double max = Math.Max(r, Math.Max(g, b)), min = Math.Min(r, Math.Min(g, b));
        v = max; double d = max - min; s = max == 0 ? 0 : d / max;
        if (d == 0) h = 0;
        else if (max == r) h = ((g - b) / d + (g < b ? 6 : 0)) / 6;
        else if (max == g) h = ((b - r) / d + 2) / 6;
        else h = ((r - g) / d + 4) / 6;
    }
    static void HsvToRgb(double h, double s, double v, out double r, out double g, out double b)
    {
        int i = (int)Math.Floor(h * 6); double f = h * 6 - i;
        double p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
        switch (i % 6) {
            case 0: r=v; g=t; b=p; break; case 1: r=q; g=v; b=p; break;
            case 2: r=p; g=v; b=t; break; case 3: r=p; g=q; b=v; break;
            case 4: r=t; g=p; b=v; break; default: r=v; g=p; b=q; break;
        }
    }
    static ushort Enhance(byte red, byte green, byte blue, byte alpha)
    {
        if (alpha < 32) return 0;
        double h,s,v,r,g,b; RgbToHsv(red/255.0, green/255.0, blue/255.0, out h,out s,out v);
        s = Math.Min(1, s * 1.18);
        if (h >= .20 && h < .48 && s > .18) { h = h*.72 + .38*.28; s=Math.Min(1,Math.Max(.68,s*1.12)); v=Math.Min(1,v*1.18); }
        else if (h >= .48 && h <= .72 && s > .18) { s=Math.Min(1,Math.Max(.76,s*1.12)); v=Math.Min(1,v*1.162); }
        HsvToRgb(h,s,v,out r,out g,out b);
        int rr=((int)Math.Round(r*255)*alpha+127)/255, gg=((int)Math.Round(g*255)*alpha+127)/255, bb=((int)Math.Round(b*255)*alpha+127)/255;
        return (ushort)(((rr & 0xf8)<<8)|((gg & 0xfc)<<3)|(bb>>3));
    }

    static byte[] Frame(Bitmap atlas, int row, int column)
    {
        using (Bitmap frame = new Bitmap(Width, Height, PixelFormat.Format32bppArgb)) {
            using (Graphics g = Graphics.FromImage(frame)) {
                g.Clear(Color.Transparent); g.CompositingMode=CompositingMode.SourceCopy;
                g.InterpolationMode=InterpolationMode.NearestNeighbor; g.PixelOffsetMode=PixelOffsetMode.Half;
                g.DrawImage(atlas, new Rectangle(0,0,Width,Height),
                    new Rectangle(column*CellWidth,row*CellHeight,CellWidth,CellHeight), GraphicsUnit.Pixel);
            }
            BitmapData bits=frame.LockBits(new Rectangle(0,0,Width,Height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
            try {
                byte[] output=new byte[Width*Height*2], scan=new byte[Math.Abs(bits.Stride)];
                for(int y=0;y<Height;y++) {
                    Marshal.Copy(IntPtr.Add(bits.Scan0,y*bits.Stride),scan,0,scan.Length);
                    for(int x=0;x<Width;x++) { int p=x*4; ushort c=Enhance(scan[p+2],scan[p+1],scan[p],scan[p+3]); int o=(y*Width+x)*2; output[o]=(byte)c; output[o+1]=(byte)(c>>8); }
                }
                return output;
            } finally { frame.UnlockBits(bits); }
        }
    }

    public static byte[] Build(string pngPath)
    {
        using (Bitmap atlas = new Bitmap(pngPath)) {
            if (atlas.Width != 1536 || (atlas.Height != 1872 && atlas.Height != 2288))
                throw new InvalidDataException("Expected a Codex 1536x1872 v1 or 1536x2288 v2 spritesheet.");
            const int headerSize=64, animationBytes=40, frameCount=30, frameTableBytes=240;
            int dataOffset=headerSize+animationBytes+frameTableBytes;
            using(MemoryStream bodyStream=new MemoryStream()) using(BinaryWriter body=new BinaryWriter(bodyStream)) {
                int first=0;
                for(int a=0;a<5;a++) { body.Write((byte)a); body.Write((byte)(Loops[a]?1:0)); body.Write((ushort)first); body.Write((ushort)Counts[a]); body.Write((ushort)0); first+=Counts[a]; }
                int absolute=0, frameSize=Width*Height*2;
                for(int a=0;a<5;a++) for(int c=0;c<Counts[a];c++) { body.Write((uint)(dataOffset+absolute*frameSize)); body.Write((ushort)Durations[a][c]); body.Write((ushort)0); absolute++; }
                for(int a=0;a<5;a++) for(int c=0;c<Counts[a];c++) body.Write(Frame(atlas,Rows[a],c));
                byte[] payload=bodyStream.ToArray(); uint contentCrc=MerryPetCrc32.Compute(payload);
                using(MemoryStream result=new MemoryStream()) using(BinaryWriter writer=new BinaryWriter(result)) {
                    writer.Write(0x3546504du); writer.Write((ushort)1); writer.Write((ushort)headerSize);
                    writer.Write((ushort)Width); writer.Write((ushort)Height); writer.Write((ushort)frameCount); writer.Write((ushort)5);
                    writer.Write((uint)headerSize); writer.Write((uint)(headerSize+animationBytes)); writer.Write((uint)dataOffset);
                    writer.Write((uint)(headerSize+payload.Length)); writer.Write(contentCrc);
                    for(int i=0;i<7;i++) writer.Write((uint)0); writer.Write(payload); return result.ToArray();
                }
            }
        }
    }
}
'@
}

function Resolve-PetSource([string]$Value, [string]$Directory) {
    if (Test-Path -LiteralPath $Value) {
        $path = (Resolve-Path -LiteralPath $Value).Path
        return [pscustomobject]@{ Id=[IO.Path]::GetFileNameWithoutExtension($path).ToLowerInvariant(); Path=$path }
    }
    $slug = $Value.Trim()
    if ($slug -match '(?i)(?:#/|/)pets/([a-z0-9_-]+)') { $slug = $Matches[1] }
    if ($slug -notmatch '^[a-z0-9_-]{1,31}$') { throw 'Enter a Codex Pets slug, pet page URL, or local spritesheet path.' }
    $metadata = Invoke-RestMethod -Uri ("https://codex-pets.net/api/pets/{0}" -f $slug) -TimeoutSec 30
    $url = [string]$metadata.spritesheetUrl
    if ([string]::IsNullOrWhiteSpace($url)) { $url = "https://codex-pets.net/assets/pets/$slug/spritesheet.webp" }
    elseif ($url.StartsWith('/')) { $url = 'https://codex-pets.net' + $url }
    if (-not $url.StartsWith('https://', [StringComparison]::OrdinalIgnoreCase)) { throw 'The pet API returned a non-HTTPS asset URL.' }
    $path = Join-Path $Directory 'spritesheet.webp'
    Invoke-WebRequest -Uri $url -OutFile $path -TimeoutSec 60
    if ((Get-Item $path).Length -gt 16MB) { throw 'The spritesheet exceeds the 16 MB safety limit.' }
    return [pscustomobject]@{ Id=$slug; Path=$path }
}

function Convert-WebpToPng([string]$InputPath, [string]$PngPath) {
    if ([IO.Path]::GetExtension($InputPath) -ieq '.png') {
        Copy-Item -LiteralPath $InputPath -Destination $PngPath -Force
        return
    }
    $decoder = Join-Path $PSScriptRoot 'libwebp\dwebp.exe'
    if (-not (Test-Path -LiteralPath $decoder)) { throw 'The bundled WebP decoder is missing.' }
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $messages = & $decoder $InputPath -o $PngPath 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousPreference
    if ($exitCode -ne 0 -or -not (Test-Path -LiteralPath $PngPath)) {
        throw ('WebP decoding failed: ' + ($messages -join ' '))
    }
}

function Read-Exact([IO.Ports.SerialPort]$Serial, [int]$Count) {
    $data=[byte[]]::new($Count); $offset=0
    while($offset -lt $Count) { $offset += $Serial.Read($data,$offset,$Count-$offset) }
    return ,$data
}
function Read-PetAck([IO.Ports.SerialPort]$Serial, [uint16]$Sequence) {
    $data=Read-Exact $Serial 8
    if ([BitConverter]::ToUInt32($data,0) -ne [uint32]0x3152504d -or
        [BitConverter]::ToUInt16($data,4) -ne $Sequence -or
        [BitConverter]::ToUInt16($data,6) -ne 0) { throw ('Pet upload rejected at sequence {0}.' -f $Sequence) }
}
function Open-MerryPort([string]$Requested) {
    $candidates = if ($Requested) { @($Requested) } else {
        @(Get-CimInstance Win32_SerialPort | Where-Object PNPDeviceID -Match '^USB\\VID_303A&PID_1001' | Select-Object -ExpandProperty DeviceID)
    }
    foreach($candidate in $candidates) {
        $serial=[IO.Ports.SerialPort]::new($candidate,115200,'None',8,'One'); $serial.ReadTimeout=15000; $serial.WriteTimeout=15000; $serial.DtrEnable=$true; $serial.RtsEnable=$false
        try {
            $serial.Open(); Start-Sleep -Milliseconds 400; $serial.DiscardInBuffer(); $magic=[BitConverter]::GetBytes([uint32]0x3149504d); $serial.Write($magic,0,4)
            $info=Read-Exact $serial 48
            if ([BitConverter]::ToUInt32($info,0) -eq [uint32]0x3141504d -and [BitConverter]::ToUInt16($info,6) -eq 0) { return $serial }
        } catch {}
        try { $serial.Dispose() } catch {}
    }
    throw 'No ESP32 Merry display answered the pet protocol.'
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('MerryPet-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($work) | Out-Null
try {
    $resolved=Resolve-PetSource $Source $work
    if ($resolved.Id -notmatch '^[a-z0-9_-]{1,31}$') { throw 'Pet id must contain only letters, digits, underscore, or hyphen (31 max).' }
    if ([IO.Path]::GetExtension($resolved.Path) -ieq '.petpack') {
        [byte[]]$pack=[IO.File]::ReadAllBytes($resolved.Path)
    } else {
        $png=Join-Path $work 'spritesheet.png'; Convert-WebpToPng $resolved.Path $png
        [byte[]]$pack=[MerryPetPackBuilder]::Build($png)
    }
    if ($OutputPack) { [IO.File]::WriteAllBytes([IO.Path]::GetFullPath($OutputPack),$pack) }
    if ($ConvertOnly) { Write-Output ("OK converted pet={0};bytes={1};crc={2:x8}" -f $resolved.Id,$pack.Length,[MerryPetCrc32]::Compute($pack)); exit 0 }

    $serial=Open-MerryPort $Port
    try {
        $id=[Text.Encoding]::ASCII.GetBytes($resolved.Id); $body=[byte[]]::new(48); $body[0]=1; [BitConverter]::GetBytes([uint16]$id.Length).CopyTo($body,2)
        [BitConverter]::GetBytes([uint32]$pack.Length).CopyTo($body,4); [BitConverter]::GetBytes([MerryPetCrc32]::Compute($pack)).CopyTo($body,8); $id.CopyTo($body,12)
        [byte[]]$crcInput=$body[0..43]; [BitConverter]::GetBytes([MerryPetCrc32]::Compute($crcInput)).CopyTo($body,44)
        $request=[byte[]]::new(52); [BitConverter]::GetBytes([uint32]0x3155504d).CopyTo($request,0); $body.CopyTo($request,4)
        $serial.DiscardInBuffer(); $serial.Write($request,0,$request.Length); Read-PetAck $serial ([uint16]0xffff)
        $offset=0; [uint16]$sequence=0
        while($offset -lt $pack.Length) {
            $size=[Math]::Min(2048,$pack.Length-$offset); $packet=[byte[]]::new(12+$size)
            [BitConverter]::GetBytes([uint32]0x3143504d).CopyTo($packet,0); [BitConverter]::GetBytes($sequence).CopyTo($packet,4); [BitConverter]::GetBytes([uint16]$size).CopyTo($packet,6)
            [BitConverter]::GetBytes([MerryPetCrc32]::Compute($pack,$offset,$size)).CopyTo($packet,8); [Array]::Copy($pack,$offset,$packet,12,$size)
            $serial.Write($packet,0,$packet.Length); Read-PetAck $serial $sequence; $offset+=$size; $sequence=[uint16](($sequence+1)-band 0xffff)
        }
        Read-PetAck $serial ([uint16]0xfffe)
    } finally { if($serial){$serial.Dispose()} }
    Write-Output ("OK installed pet={0};bytes={1};verified=yes" -f $resolved.Id,$pack.Length)
}
finally { if(Test-Path -LiteralPath $work){Remove-Item -LiteralPath $work -Recurse -Force} }
