[CmdletBinding()]
param(
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [string]$LogDirectory = (Join-Path $env:LOCALAPPDATA 'MerryDongle\Logs'),

    [ValidateRange(2, 20)]
    [int]$RetainLogs = 5
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$mutex = [Threading.Mutex]::new($false, 'Local\MerryDongleBridge')
$ownsMutex = $false
$transcriptStarted = $false
$exitCode = 0

try {
    try {
        $ownsMutex = $mutex.WaitOne(0)
    }
    catch [Threading.AbandonedMutexException] {
        $ownsMutex = $true
    }
    if (-not $ownsMutex) {
        return
    }

    New-Item -ItemType Directory -Path $LogDirectory -Force | Out-Null
    Get-ChildItem -LiteralPath $LogDirectory -Filter 'merry-bridge-*.log' -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -Skip ($RetainLogs - 1) |
        Remove-Item -Force

    $logPath = Join-Path $LogDirectory (
        'merry-bridge-{0:yyyyMMdd-HHmmss}.log' -f [DateTime]::Now
    )
    Start-Transcript -LiteralPath $logPath -Force | Out-Null
    $transcriptStarted = $true

    $bridgePath = Join-Path $PSScriptRoot 'merry_codex_bridge.ps1'
    $parameters = @{}
    if ($PSBoundParameters.ContainsKey('Port')) {
        $parameters.Port = $Port
    }
    & $bridgePath @parameters
    throw 'Merry bridge exited unexpectedly.'
}
catch {
    Write-Error -ErrorRecord $_ -ErrorAction Continue
    $exitCode = 1
}
finally {
    if ($transcriptStarted) {
        try { Stop-Transcript | Out-Null } catch {}
    }
    if ($ownsMutex) {
        try { $mutex.ReleaseMutex() } catch {}
    }
    $mutex.Dispose()
}

exit $exitCode
