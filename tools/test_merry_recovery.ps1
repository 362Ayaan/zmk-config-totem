[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [ValidateRange(250, 5000)]
    [int]$VisualDelayMilliseconds = 900,

    [switch]$SkipSpotify
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'merry_codex_bridge.ps1') -SelfTest -Port $Port

function Wait-Visual {
    Start-Sleep -Milliseconds $VisualDelayMilliseconds
}

$passed = [Collections.Generic.List[string]]::new()

try {
    Connect-Dongle
    $passed.Add('Initial protocol connection')

    foreach ($state in @('Idle', 'Running', 'NeedsInput', 'Completed', 'Blocked')) {
        Invoke-DongleState -Serial $script:serial -State $state
        $passed.Add("Codex state: $state")
        Wait-Visual
    }

    Invoke-DongleHostState -Serial $script:serial -State 'Active'
    $passed.Add('Host state: Active')
    Wait-Visual
    Invoke-DongleHostState -Serial $script:serial -State 'DisplayOff'
    $passed.Add('Host state: DisplayOff')
    Wait-Visual
    Invoke-DongleHostState -Serial $script:serial -State 'Active'
    $passed.Add('Host wake after DisplayOff')
    Wait-Visual
    Invoke-DongleHostState -Serial $script:serial -State 'AFK'
    $passed.Add('Host state: AFK')

    Disconnect-Dongle
    Start-Sleep -Milliseconds 500
    Connect-Dongle
    $passed.Add('Software disconnect/reconnect')

    if (-not $SkipSpotify) {
        $playback = Get-MerrySpotifyPlayback
        $snapshot = Get-MerrySpotifyTrack -Playback $playback
        if (-not [string]::IsNullOrWhiteSpace($snapshot.Key)) {
            $image = ConvertTo-MerryAlbumBytes -Snapshot $snapshot
            Invoke-DongleMediaUpload -Serial $script:serial -Image $image -State 'Paused'
            $passed.Add('Spotify paused artwork upload')
            Wait-Visual
            Invoke-DongleState -Serial $script:serial -State 'Running'
            $passed.Add('Spotify preempted by Codex Running')
            Wait-Visual
            Invoke-DongleState -Serial $script:serial -State 'Completed'
            Invoke-DongleMediaUpload -Serial $script:serial -Image $image -State 'Paused'
            $passed.Add('Spotify restored after Codex Completed')
            Wait-Visual
            Invoke-DongleMediaState -Serial $script:serial -State 'Playing'
            Invoke-DongleMediaState -Serial $script:serial -State 'Paused'
            $passed.Add('Spotify icon-only play/pause updates')
        } else {
            Write-Warning 'Spotify has no artwork-bearing media session; media recovery was skipped.'
        }
    }

    Invoke-DongleState -Serial $script:serial -State 'Idle'
    Invoke-DongleMediaState -Serial $script:serial -State 'None'
    Invoke-DongleHostState -Serial $script:serial -State 'Active'
}
finally {
    Disconnect-Dongle
}

Write-Host "Merry automated recovery matrix passed ($($passed.Count) checks):"
$passed | ForEach-Object { Write-Host "  PASS  $_" }
