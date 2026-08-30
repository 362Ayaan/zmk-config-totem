[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [switch]$Clear
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$statusMagic = [uint32]0x3154534d   # MST1
$clearMagic = [uint32]0x314c434d    # MCL1
$responseMagic = [uint32]0x3152534d # MSR1

function Read-Exact {
    param([System.IO.Ports.SerialPort]$Serial, [int]$Count)
    $buffer = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $offset += $Serial.Read($buffer, $offset, $Count - $offset)
    }
    return $buffer
}

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.ReadTimeout = 60000
$serial.WriteTimeout = 5000
$serial.DtrEnable = $true
$serial.RtsEnable = $false

try {
    $serial.Open()
    Start-Sleep -Milliseconds 750
    $serial.DiscardInBuffer()
    $serial.DiscardOutBuffer()

    $command = [BitConverter]::GetBytes($(if ($Clear) { $clearMagic } else { $statusMagic }))
    $serial.Write($command, 0, $command.Length)
    if ($Clear) {
        Write-Host 'Erasing both Merry upload slots; embedded fallback remains available...'
    }

    $response = Read-Exact -Serial $serial -Count 24
    $magic = [BitConverter]::ToUInt32($response, 0)
    $status = [BitConverter]::ToUInt16($response, 4)
    if ($magic -ne $responseMagic) {
        throw ('Dongle returned invalid store-response magic 0x{0:x8}.' -f $magic)
    }
    if ($status -ne 0) {
        throw "Dongle store command failed with status $status."
    }

    $slot = [int]$response[10]
    [pscustomobject]@{
        PackVersion = [int]$response[8]
        UploadedPackActive = [bool]$response[9]
        ActiveSlot = $(if ($slot -eq 255) { 'None' } else { $slot })
        Quarantined = [bool]$response[11]
        Generation = [BitConverter]::ToUInt32($response, 12)
        PackBytes = [BitConverter]::ToUInt32($response, 16)
        LastError = [BitConverter]::ToInt32($response, 20)
    } | Format-List
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
