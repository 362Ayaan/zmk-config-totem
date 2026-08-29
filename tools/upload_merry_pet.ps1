[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [string]$PetPack = (Join-Path $PSScriptRoot '..\assets\merry\merry.petpack')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not ('MerryCrc32' -as [type])) {
    Add-Type -TypeDefinition @'
using System;

public static class MerryCrc32
{
    public static uint Compute(byte[] data, int offset, int count)
    {
        uint crc = 0xffffffffU;
        for (int index = 0; index < count; index++)
        {
            crc ^= data[offset + index];
            for (int bit = 0; bit < 8; bit++)
            {
                crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xedb88320U : 0U);
            }
        }
        return crc ^ 0xffffffffU;
    }
}
'@
}

$uploadMagic = [uint32]0x3150554d # MUP1
$chunkMagic = [uint32]0x3148434d  # MCH1
$responseMagic = [uint32]0x3153524d # MRS1
$readySequence = [uint16]0xffff
$finalSequence = [uint16]0xfffe
$chunkSize = 512

function Add-UInt16LE {
    param([System.Collections.Generic.List[byte]]$Bytes, [uint16]$Value)
    $Bytes.AddRange([BitConverter]::GetBytes($Value))
}

function Add-UInt32LE {
    param([System.Collections.Generic.List[byte]]$Bytes, [uint32]$Value)
    $Bytes.AddRange([BitConverter]::GetBytes($Value))
}

function Read-Exact {
    param([System.IO.Ports.SerialPort]$Serial, [int]$Count)
    $buffer = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $offset += $Serial.Read($buffer, $offset, $Count - $offset)
    }
    return $buffer
}

function Read-Response {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [uint16]$ExpectedSequence
    )
    $response = Read-Exact -Serial $Serial -Count 8
    $magic = [BitConverter]::ToUInt32($response, 0)
    $sequence = [BitConverter]::ToUInt16($response, 4)
    $status = [BitConverter]::ToUInt16($response, 6)
    if ($magic -ne $responseMagic) {
        throw ('Dongle returned an invalid response magic: 0x{0:x8}' -f $magic)
    }
    if ($sequence -ne $ExpectedSequence) {
        throw "Dongle acknowledged sequence $sequence; expected $ExpectedSequence."
    }
    if ($status -ne 0) {
        throw "Dongle rejected sequence $sequence with status $status. The previous pet remains valid."
    }
}

$resolvedPack = (Resolve-Path -LiteralPath $PetPack).Path
$pack = [IO.File]::ReadAllBytes($resolvedPack)
if ($pack.Length -gt (1024 * 1024 - 4096)) {
    throw "Pet pack is too large for one atomic slot: $($pack.Length) bytes."
}
if (($pack.Length % 4) -ne 0) {
    throw "Pet pack length must be a multiple of four bytes for QSPI writes."
}
$packCrc = [MerryCrc32]::Compute($pack, 0, $pack.Length)

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.ReadTimeout = 60000
$serial.WriteTimeout = 10000
$serial.DtrEnable = $true
$serial.RtsEnable = $false

try {
    $serial.Open()
    Start-Sleep -Milliseconds 750
    $serial.DiscardInBuffer()
    $serial.DiscardOutBuffer()

    $header = [System.Collections.Generic.List[byte]]::new(16)
    Add-UInt32LE $header $uploadMagic
    Add-UInt32LE $header ([uint32]$pack.Length)
    Add-UInt32LE $header $packCrc
    Add-UInt32LE $header 0
    $headerBytes = $header.ToArray()
    $serial.Write($headerBytes, 0, $headerBytes.Length)

    Write-Host "Erasing the inactive QSPI slot; the current pet remains available..."
    Read-Response -Serial $serial -ExpectedSequence $readySequence

    [uint16]$sequence = 0
    for ($offset = 0; $offset -lt $pack.Length; $offset += $chunkSize) {
        $length = [Math]::Min($chunkSize, $pack.Length - $offset)
        $chunkCrc = [MerryCrc32]::Compute($pack, $offset, $length)
        $chunkHeader = [System.Collections.Generic.List[byte]]::new(12)
        Add-UInt32LE $chunkHeader $chunkMagic
        Add-UInt16LE $chunkHeader $sequence
        Add-UInt16LE $chunkHeader ([uint16]$length)
        Add-UInt32LE $chunkHeader $chunkCrc
        $chunkHeaderBytes = $chunkHeader.ToArray()

        $serial.Write($chunkHeaderBytes, 0, $chunkHeaderBytes.Length)
        $serial.Write($pack, $offset, $length)
        Read-Response -Serial $serial -ExpectedSequence $sequence

        $percent = [int](100 * ($offset + $length) / $pack.Length)
        Write-Progress -Activity 'Uploading Merry' -Status "$percent%" -PercentComplete $percent
        $sequence++
    }

    Read-Response -Serial $serial -ExpectedSequence $finalSequence
    Write-Progress -Activity 'Uploading Merry' -Completed
    Write-Host ('Merry installed atomically: {0:N0} bytes, CRC32 {1:x8}.' -f $pack.Length, $packCrc)
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
