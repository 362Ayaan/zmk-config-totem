[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [ValidateSet('Auto', 'Dashboard', 'Pet', 'Off')]
    [string]$Mode,

    [ValidateSet('Idle', 'Running', 'NeedsInput', 'Completed', 'Blocked')]
    [string]$Animation,

    [ValidateRange(0, 100)]
    [int]$Brightness,

    [ValidateRange(1, 3600)]
    [double]$TimeoutSeconds,

    [ValidateRange(-40, 40)]
    [int]$PetX,

    [ValidateRange(-33, 33)]
    [int]$PetY,

    [ValidateRange(0, 2)]
    [int]$BatteryLeftSlot,

    [ValidateRange(0, 2)]
    [int]$BatteryDialSlot,

    [ValidateRange(0, 2)]
    [int]$BatteryRightSlot,

    [switch]$Reset
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not ('MerryConfigCrc32' -as [type])) {
    Add-Type -TypeDefinition @'
using System;

public static class MerryConfigCrc32
{
    public static uint Compute(byte[] data)
    {
        uint crc = 0xffffffffU;
        for (int index = 0; index < data.Length; index++)
        {
            crc ^= data[index];
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

$configMagic = [uint32]0x3146434d         # MCF1
$configResponseMagic = [uint32]0x3152434d # MCR1
$modeValues = @{ Auto = 0; Dashboard = 1; Pet = 2; Off = 3 }
$modeNames = @('Auto', 'Dashboard', 'Pet', 'Off')
$animationValues = @{
    Idle = 0; Running = 1; NeedsInput = 2; Completed = 3; Blocked = 4
}
$animationNames = @('Idle', 'Running', 'NeedsInput', 'Completed', 'Blocked')

function Read-Exact {
    param([System.IO.Ports.SerialPort]$Serial, [int]$Count)
    $buffer = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $offset += $Serial.Read($buffer, $offset, $Count - $offset)
    }
    return $buffer
}

function New-DefaultConfig {
    $bytes = [byte[]]::new(16)
    $bytes[0] = 3
    $bytes[3] = 100
    [BitConverter]::GetBytes([uint32]20000).CopyTo($bytes, 4)
    $bytes[12] = 2
    $bytes[13] = 0
    $bytes[14] = 1
    return $bytes
}

function Invoke-ConfigCommand {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [ValidateRange(0, 2)][int]$Operation,
        [byte[]]$Config
    )

    $request = [byte[]]::new(28)
    [BitConverter]::GetBytes($configMagic).CopyTo($request, 0)
    $request[4] = [byte]$Operation
    $Config.CopyTo($request, 8)
    $crc = [MerryConfigCrc32]::Compute($Config)
    [BitConverter]::GetBytes($crc).CopyTo($request, 24)
    $Serial.Write($request, 0, $request.Length)

    $response = Read-Exact -Serial $Serial -Count 28
    $magic = [BitConverter]::ToUInt32($response, 0)
    $status = [BitConverter]::ToUInt16($response, 4)
    if ($magic -ne $configResponseMagic) {
        throw ('Dongle returned invalid config-response magic 0x{0:x8}.' -f $magic)
    }
    if ($status -ne 0) {
        throw "Dongle rejected the config command with status $status."
    }

    $result = [byte[]]::new(16)
    [Array]::Copy($response, 8, $result, 0, 16)
    $expectedCrc = [BitConverter]::ToUInt32($response, 24)
    $actualCrc = [MerryConfigCrc32]::Compute($result)
    if ($actualCrc -ne $expectedCrc) {
        throw 'Dongle config response failed its CRC check.'
    }
    return $result
}

function Show-Config {
    param([byte[]]$Config)
    $modeIndex = [int]$Config[1]
    $animationIndex = [int]$Config[2]
    [pscustomobject]@{
        Mode = $modeNames[$modeIndex]
        Animation = $animationNames[$animationIndex]
        Brightness = "{0}%" -f $Config[3]
        TimeoutSeconds = [BitConverter]::ToUInt32($Config, 4) / 1000.0
        PetX = [BitConverter]::ToInt16($Config, 8)
        PetY = [BitConverter]::ToInt16($Config, 10)
        BatteryLeftSlot = $Config[12]
        BatteryDialSlot = $Config[13]
        BatteryRightSlot = $Config[14]
    } | Format-List
}

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.ReadTimeout = 5000
$serial.WriteTimeout = 5000
$serial.DtrEnable = $true
$serial.RtsEnable = $false

try {
    $serial.Open()
    Start-Sleep -Milliseconds 750
    $serial.DiscardInBuffer()
    $serial.DiscardOutBuffer()

    if ($Reset) {
        $config = Invoke-ConfigCommand -Serial $serial -Operation 2 -Config (New-DefaultConfig)
        Write-Host 'Merry settings reset and saved.'
        Show-Config $config
        return
    }

    $config = Invoke-ConfigCommand -Serial $serial -Operation 0 -Config (New-DefaultConfig)
    $hasChanges = $PSBoundParameters.ContainsKey('Mode') -or
                  $PSBoundParameters.ContainsKey('Animation') -or
                  $PSBoundParameters.ContainsKey('Brightness') -or
                  $PSBoundParameters.ContainsKey('TimeoutSeconds') -or
                  $PSBoundParameters.ContainsKey('PetX') -or
                  $PSBoundParameters.ContainsKey('PetY') -or
                  $PSBoundParameters.ContainsKey('BatteryLeftSlot') -or
                  $PSBoundParameters.ContainsKey('BatteryDialSlot') -or
                  $PSBoundParameters.ContainsKey('BatteryRightSlot')

    if ($PSBoundParameters.ContainsKey('Mode')) {
        $config[1] = [byte]$modeValues[$Mode]
    }
    if ($PSBoundParameters.ContainsKey('Animation')) {
        $config[2] = [byte]$animationValues[$Animation]
    }
    if ($PSBoundParameters.ContainsKey('Brightness')) {
        $config[3] = [byte]$Brightness
    }
    if ($PSBoundParameters.ContainsKey('TimeoutSeconds')) {
        $timeoutMs = [uint32][Math]::Round($TimeoutSeconds * 1000)
        [BitConverter]::GetBytes($timeoutMs).CopyTo($config, 4)
    }
    if ($PSBoundParameters.ContainsKey('PetX')) {
        [BitConverter]::GetBytes([int16]$PetX).CopyTo($config, 8)
    }
    if ($PSBoundParameters.ContainsKey('PetY')) {
        [BitConverter]::GetBytes([int16]$PetY).CopyTo($config, 10)
    }
    if ($PSBoundParameters.ContainsKey('BatteryLeftSlot')) {
        $config[12] = [byte]$BatteryLeftSlot
    }
    if ($PSBoundParameters.ContainsKey('BatteryDialSlot')) {
        $config[13] = [byte]$BatteryDialSlot
    }
    if ($PSBoundParameters.ContainsKey('BatteryRightSlot')) {
        $config[14] = [byte]$BatteryRightSlot
    }
    $uniqueBatterySlots = @($config[12], $config[13], $config[14]) |
        Select-Object -Unique
    if ($uniqueBatterySlots.Count -ne 3) {
        throw 'BatteryLeftSlot, BatteryDialSlot, and BatteryRightSlot must be distinct.'
    }

    if ($hasChanges) {
        $config = Invoke-ConfigCommand -Serial $serial -Operation 1 -Config $config
        Write-Host 'Merry settings saved and applied immediately.'
    } else {
        Write-Host 'Current Merry settings:'
    }
    Show-Config $config
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
