[CmdletBinding()]
param(
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [string]$TaskName = 'Merry Dongle Bridge',

    [switch]$StartNow,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'The Merry startup task is supported only on Windows.'
}

$existing = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($Uninstall) {
    if ($null -ne $existing) {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "Removed scheduled task '$TaskName'."
    } else {
        Write-Host "Scheduled task '$TaskName' is not installed."
    }
    return
}

$runner = (Resolve-Path (Join-Path $PSScriptRoot 'start_merry_bridge.ps1')).Path
$workingDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$windowsPowerShell = Join-Path $env:SystemRoot `
    'System32\WindowsPowerShell\v1.0\powershell.exe'
$arguments = '-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden ' +
    '-ExecutionPolicy Bypass -File "{0}"' -f $runner
if ($PSBoundParameters.ContainsKey('Port')) {
    $arguments += ' -Port {0}' -f $Port
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute $windowsPowerShell -Argument $arguments `
    -WorkingDirectory $workingDirectory
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity
$principal = New-ScheduledTaskPrincipal -UserId $identity -LogonType Interactive `
    -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
$task = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Description (
        'Runs the Merry Codex/Spotify bridge in the interactive user session. ' +
        'The task restarts after unexpected failures and the runner rejects duplicates.'
    )

Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
Write-Host "Installed scheduled task '$TaskName' for $identity."
if ($PSBoundParameters.ContainsKey('Port')) {
    Write-Host "The task is pinned to $Port."
} else {
    Write-Host 'The task will discover and protocol-probe the dongle COM port automatically.'
}

if ($StartNow) {
    Start-ScheduledTask -TaskName $TaskName
    Write-Host 'Startup task launched.'
}
