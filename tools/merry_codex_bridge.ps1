[CmdletBinding()]
param(
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [ValidateRange(1, 30)]
    [int]$PollSeconds = 2,

    [ValidateRange(100, 2000)]
    [int]$MediaPollMilliseconds = 250,

    [ValidateRange(1, 30)]
    [int]$MediaMissingGraceSeconds = 5,

    [ValidateRange(5, 60)]
    [int]$TtlSeconds = 12,

    [ValidateRange(1, 60)]
    [int]$CompletedSeconds = 30,

    [ValidateRange(1, 120)]
    [int]$FailureSeconds = 20,

    [ValidateRange(5, 300)]
    [int]$HostActivitySeconds = 30,

    [switch]$DryRun,
    [switch]$Once,
    [switch]$SelfTest,
    [switch]$DisableSpotify
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$windowsDesktop = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT -and
    $PSVersionTable.PSEdition -eq 'Desktop'
$spotifyEnabled = -not $DisableSpotify -and $windowsDesktop
if (-not $DisableSpotify -and -not $windowsDesktop) {
    Write-Warning 'Spotify artwork requires Windows PowerShell 5.1; Spotify support is disabled in this host.'
}
if ($windowsDesktop) {
    . (Join-Path $PSScriptRoot 'merry_media_helpers.ps1')
}

if (-not ('MerryCodexCrc32' -as [type])) {
    Add-Type -TypeDefinition @'
using System;

public static class MerryCodexCrc32
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

$stateIds = [ordered]@{
    Idle = 0
    Running = 1
    NeedsInput = 2
    Completed = 3
    Blocked = 4
}
$stateNames = @('Idle', 'Running', 'NeedsInput', 'Completed', 'Blocked')
$codexStateMagic = [uint32]0x3153434d    # MCS1
$codexResponseMagic = [uint32]0x3141434d # MCA1
$codexStateVersion = [byte]1
$mediaUploadMagic = [uint32]0x3155414d   # MAU1
$mediaChunkMagic = [uint32]0x3143414d    # MAC1
$mediaResponseMagic = [uint32]0x3152414d # MAR1
$mediaStateMagic = [uint32]0x31534d4d    # MMS1
$mediaStateResponseMagic = [uint32]0x31414d4d # MMA1
$hostStateMagic = [uint32]0x3153484d        # MHS1
$hostStateResponseMagic = [uint32]0x3141484d # MHA1
$mediaProtocolVersion = [byte]1
$mediaWidth = [uint16]202
$mediaHeight = [uint16]220
$mediaBytes = 202 * 220 * 2
$mediaStateIds = @{ None = 0; Playing = 1; Paused = 2 }
$script:requestId = 0
$script:sequence = [uint16]0
$script:mediaSequence = [uint16]0
$script:mediaGeneration = [uint32]0
$script:hostSequence = [uint16]0
$script:pipe = $null
$script:toolCatalog = @{}
$script:serial = $null
$script:selectedPort = $null
$script:contextThreadId = $null

function Read-Exact {
    param([System.IO.Stream]$Stream, [int]$Count, [int]$TimeoutMs = 10000)

    $buffer = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $task = $Stream.ReadAsync($buffer, $offset, $Count - $offset)
        if (-not $task.Wait($TimeoutMs)) {
            throw "Timed out reading $Count bytes."
        }
        $read = $task.Result
        if ($read -le 0) {
            throw 'The stream closed unexpectedly.'
        }
        $offset += $read
    }
    return ,$buffer
}

function Get-CodexPipeName {
    $process = Get-CimInstance Win32_Process -Filter "Name='codex.exe'" |
        Where-Object CommandLine -Match 'CODEX_APP_TOOLS_PIPE_PATH' |
        Select-Object -First 1
    if ($null -eq $process) {
        throw 'Codex Desktop is not running or its app-tools pipe is unavailable.'
    }

    $match = [regex]::Match(
        $process.CommandLine,
        'codex-[a-z0-9-]+-[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase
    )
    if (-not $match.Success) {
        throw 'Could not extract the Codex Desktop pipe name.'
    }
    return $match.Value
}

function Get-ContextThreadId {
    $sessionRoot = Join-Path $env:USERPROFILE '.codex\sessions'
    $idPattern = '[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}'
    $session = Get-ChildItem -LiteralPath $sessionRoot -Recurse -Filter 'rollout-*.jsonl' -File |
        Sort-Object LastWriteTime -Descending |
        Where-Object Name -Match $idPattern |
        Select-Object -First 1
    if ($null -eq $session) {
        throw 'No local Codex task was found to provide read-only app-tool context.'
    }
    return [regex]::Match($session.Name, $idPattern).Value
}

function Disconnect-CodexPipe {
    $pipe = $script:pipe
    $script:pipe = $null
    if ($null -ne $pipe) {
        try { $pipe.Dispose() } catch {}
    }
    $script:toolCatalog = @{}
}

function Invoke-CodexPipeRequest {
    param([string]$Method, [hashtable]$Params)

    if ($null -eq $script:pipe -or -not $script:pipe.IsConnected) {
        throw 'Codex Desktop pipe is not connected.'
    }

    $script:requestId++
    $request = @{
        id = $script:requestId
        jsonrpc = '2.0'
        method = $Method
        params = $Params
    } | ConvertTo-Json -Compress -Depth 30
    $payload = [Text.Encoding]::UTF8.GetBytes($request)
    $length = [BitConverter]::GetBytes([uint32]$payload.Length)
    $script:pipe.Write($length, 0, $length.Length)
    $script:pipe.Write($payload, 0, $payload.Length)
    $script:pipe.Flush()

    $header = Read-Exact -Stream $script:pipe -Count 4
    $responseLength = [BitConverter]::ToUInt32($header, 0)
    if ($responseLength -eq 0 -or $responseLength -gt 8MB) {
        throw "Codex Desktop returned an invalid frame length: $responseLength."
    }
    $responseBytes = Read-Exact -Stream $script:pipe -Count $responseLength
    $response = [Text.Encoding]::UTF8.GetString($responseBytes) | ConvertFrom-Json
    if ($response.PSObject.Properties.Name -contains 'error') {
        throw "Codex Desktop request failed: $($response.error.message)"
    }
    return $response.result
}

function Connect-CodexPipe {
    Disconnect-CodexPipe
    $pipeName = Get-CodexPipeName
    $script:pipe = [IO.Pipes.NamedPipeClientStream]::new(
        '.', $pipeName, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous
    )
    $script:pipe.Connect(3000)
    $catalog = Invoke-CodexPipeRequest -Method 'tools/list' -Params @{ threadStartKind = 'all' }
    foreach ($tool in $catalog.tools) {
        $script:toolCatalog[$tool.name] = $tool
    }
    if (-not $script:toolCatalog.ContainsKey('list_threads') -or
        -not $script:toolCatalog.ContainsKey('read_thread')) {
        throw 'This Codex Desktop version does not expose the required status tools.'
    }
    $script:contextThreadId = Get-ContextThreadId
    Write-Verbose "Using Codex task context $script:contextThreadId."
}

function Invoke-CodexAppTool {
    param([string]$Name, [hashtable]$Arguments)

    $tool = $script:toolCatalog[$Name]
    $result = Invoke-CodexPipeRequest -Method 'tools/call' -Params @{
        arguments = $Arguments
        callId = "merry-$PID-$($script:requestId + 1)"
        namespace = $tool.namespace
        threadId = $script:contextThreadId
        turnId = "merry-status-$PID"
        tool = $Name
    }
    if (-not $result.success) {
        $detail = ($result.contentItems | Where-Object type -EQ 'inputText' |
            Select-Object -First 1).text
        throw "Codex app tool '$Name' reported failure: $detail"
    }
    $text = $result.contentItems | Where-Object type -EQ 'inputText' | Select-Object -First 1
    if ($null -eq $text -or [string]::IsNullOrWhiteSpace($text.text)) {
        throw "Codex app tool '$Name' returned no JSON payload."
    }
    return $text.text | ConvertFrom-Json
}

function Get-CodexTasks {
    $result = Invoke-CodexAppTool -Name 'list_threads' -Arguments @{ limit = 50 }
    return @($result.pinnedThreads) + @($result.threads) |
        Where-Object kind -EQ 'codex'
}

function Get-LastTurnOutcome {
    param([string]$ThreadId)

    try {
        $result = Invoke-CodexAppTool -Name 'read_thread' -Arguments @{
            threadId = $ThreadId
            turnLimit = 1
            includeOutputs = $false
            maxOutputCharsPerItem = 1
        }
        $turn = @($result.turns) | Select-Object -First 1
        $threadStatus = $result.thread.status.type
        $turnStatus = if ($null -ne $turn) { $turn.status } else { $null }
        $turnError = if ($null -ne $turn) { $turn.error } else { $null }
        if ($threadStatus -eq 'systemError' -or $turnStatus -eq 'failed' -or
            $null -ne $turnError) {
            return 'Blocked'
        }
        return 'Completed'
    }
    catch {
        Write-Verbose "Could not inspect completion for $ThreadId`: $($_.Exception.Message)"
        return 'Completed'
    }
}

function Get-DongleCandidates {
    if (-not [string]::IsNullOrWhiteSpace($Port)) {
        return @($Port)
    }
    return @(Get-CimInstance Win32_SerialPort |
        Where-Object PNPDeviceID -Match '^USB\\VID_1D50&PID_615E' |
        Select-Object -ExpandProperty DeviceID)
}

function Disconnect-Dongle {
    $serial = $script:serial
    $script:serial = $null
    $script:selectedPort = $null
    if ($null -ne $serial) {
        try {
            if ($serial.IsOpen) { $serial.Close() }
        } catch {}
        try { $serial.Dispose() } catch {}
    }
}

function Test-MerryMediaEligible {
    param([string]$CodexState, [string]$MediaState)
    return $MediaState -in @('Playing', 'Paused') -and $CodexState -ne 'Running'
}

function Test-MerryArtworkUploadNeeded {
    param(
        [bool]$Eligible,
        [string]$MediaKey,
        [string]$LastUploadedKey,
        [bool]$WasEligible
    )
    return $Eligible -and -not [string]::IsNullOrWhiteSpace($MediaKey) -and
        ($MediaKey -ne $LastUploadedKey -or -not $WasEligible)
}

function Invoke-DongleState {
    param([System.IO.Ports.SerialPort]$Serial, [string]$State)

    $requestBody = [byte[]]::new(8)
    $requestBody[0] = $codexStateVersion
    $requestBody[1] = [byte]$stateIds[$State]
    [BitConverter]::GetBytes($script:sequence).CopyTo($requestBody, 2)
    [BitConverter]::GetBytes([uint32]($TtlSeconds * 1000)).CopyTo($requestBody, 4)
    $crc = [MerryCodexCrc32]::Compute($requestBody)
    $request = [byte[]]::new(16)
    [BitConverter]::GetBytes($codexStateMagic).CopyTo($request, 0)
    $requestBody.CopyTo($request, 4)
    [BitConverter]::GetBytes($crc).CopyTo($request, 12)

    $Serial.DiscardInBuffer()
    $Serial.Write($request, 0, $request.Length)
    $response = [byte[]]::new(8)
    $offset = 0
    while ($offset -lt $response.Length) {
        $offset += $Serial.Read($response, $offset, $response.Length - $offset)
    }
    $magic = [BitConverter]::ToUInt32($response, 0)
    $status = $response[4]
    $appliedState = $response[5]
    $responseSequence = [BitConverter]::ToUInt16($response, 6)
    if ($magic -ne $codexResponseMagic -or $status -ne 0 -or
        $responseSequence -ne $script:sequence -or $appliedState -ne $stateIds[$State]) {
        throw ('Invalid dongle acknowledgement: magic=0x{0:x8}, status={1}, state={2}, sequence={3}.' -f
            $magic, $status, $appliedState, $responseSequence)
    }
    $script:sequence = [uint16](($script:sequence + 1) -band 0xffff)
}

function Read-MerryMediaResponse {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [uint16]$ExpectedSequence
    )

    $response = [byte[]]::new(8)
    $offset = 0
    while ($offset -lt $response.Length) {
        $offset += $Serial.Read($response, $offset, $response.Length - $offset)
    }
    $magic = [BitConverter]::ToUInt32($response, 0)
    $sequence = [BitConverter]::ToUInt16($response, 4)
    $status = [BitConverter]::ToUInt16($response, 6)
    if ($magic -ne $mediaResponseMagic -or $sequence -ne $ExpectedSequence -or $status -ne 0) {
        throw ('Invalid media acknowledgement: magic=0x{0:x8}, sequence={1}, status={2}.' -f
            $magic, $sequence, $status)
    }
}

function Invoke-DongleMediaUpload {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [byte[]]$Image,
        [string]$State
    )

    if ($Image.Length -ne $mediaBytes -or -not $mediaStateIds.ContainsKey($State) -or
        $State -eq 'None') {
        throw 'The media image or playback state is invalid.'
    }
    $script:mediaGeneration = [uint32](($script:mediaGeneration + 1) -band 0xffffffffL)
    $imageCrc = [MerryCodexCrc32]::Compute($Image)
    $request = [byte[]]::new(24)
    [BitConverter]::GetBytes($mediaUploadMagic).CopyTo($request, 0)
    $request[4] = $mediaProtocolVersion
    $request[5] = [byte]$mediaStateIds[$State]
    [BitConverter]::GetBytes($mediaWidth).CopyTo($request, 6)
    [BitConverter]::GetBytes($mediaHeight).CopyTo($request, 8)
    [BitConverter]::GetBytes([uint16]0).CopyTo($request, 10)
    [BitConverter]::GetBytes([uint32]$Image.Length).CopyTo($request, 12)
    [BitConverter]::GetBytes($imageCrc).CopyTo($request, 16)
    [BitConverter]::GetBytes($script:mediaGeneration).CopyTo($request, 20)

    $Serial.DiscardInBuffer()
    $Serial.Write($request, 0, $request.Length)
    Read-MerryMediaResponse -Serial $Serial -ExpectedSequence ([uint16]0xffff)

    $offset = 0
    $sequence = [uint16]0
    while ($offset -lt $Image.Length) {
        $size = [Math]::Min(512, $Image.Length - $offset)
        $payload = [byte[]]::new($size)
        [Array]::Copy($Image, $offset, $payload, 0, $size)
        $packet = [byte[]]::new(12 + $size)
        [BitConverter]::GetBytes($mediaChunkMagic).CopyTo($packet, 0)
        [BitConverter]::GetBytes($sequence).CopyTo($packet, 4)
        [BitConverter]::GetBytes([uint16]$size).CopyTo($packet, 6)
        [BitConverter]::GetBytes([MerryCodexCrc32]::Compute($payload)).CopyTo($packet, 8)
        $payload.CopyTo($packet, 12)
        $Serial.Write($packet, 0, $packet.Length)
        Read-MerryMediaResponse -Serial $Serial -ExpectedSequence $sequence
        $offset += $size
        $sequence = [uint16](($sequence + 1) -band 0xffff)
    }
    Read-MerryMediaResponse -Serial $Serial -ExpectedSequence ([uint16]0xfffe)
}

function Invoke-DongleMediaState {
    param([System.IO.Ports.SerialPort]$Serial, [string]$State)

    $body = [byte[]]::new(8)
    $body[0] = $mediaProtocolVersion
    $body[1] = [byte]$mediaStateIds[$State]
    [BitConverter]::GetBytes($script:mediaSequence).CopyTo($body, 2)
    [BitConverter]::GetBytes([uint32]($TtlSeconds * 1000)).CopyTo($body, 4)
    $request = [byte[]]::new(16)
    [BitConverter]::GetBytes($mediaStateMagic).CopyTo($request, 0)
    $body.CopyTo($request, 4)
    [BitConverter]::GetBytes([MerryCodexCrc32]::Compute($body)).CopyTo($request, 12)

    $Serial.Write($request, 0, $request.Length)
    $response = [byte[]]::new(8)
    $offset = 0
    while ($offset -lt $response.Length) {
        $offset += $Serial.Read($response, $offset, $response.Length - $offset)
    }
    $magic = [BitConverter]::ToUInt32($response, 0)
    $status = $response[4]
    $appliedState = $response[5]
    $sequence = [BitConverter]::ToUInt16($response, 6)
    if ($magic -ne $mediaStateResponseMagic -or $status -ne 0 -or
        $sequence -ne $script:mediaSequence -or $appliedState -ne $mediaStateIds[$State]) {
        throw ('Invalid media-state acknowledgement: magic=0x{0:x8}, status={1}, state={2}, sequence={3}.' -f
            $magic, $status, $appliedState, $sequence)
    }
    $script:mediaSequence = [uint16](($script:mediaSequence + 1) -band 0xffff)
}

function Invoke-DongleHostActivity {
    param([System.IO.Ports.SerialPort]$Serial, [bool]$Active)

    $body = [byte[]]::new(8)
    $body[0] = $mediaProtocolVersion
    $body[1] = if ($Active) { [byte]1 } else { [byte]0 }
    [BitConverter]::GetBytes($script:hostSequence).CopyTo($body, 2)
    [BitConverter]::GetBytes([uint32]($TtlSeconds * 1000)).CopyTo($body, 4)
    $request = [byte[]]::new(16)
    [BitConverter]::GetBytes($hostStateMagic).CopyTo($request, 0)
    $body.CopyTo($request, 4)
    [BitConverter]::GetBytes([MerryCodexCrc32]::Compute($body)).CopyTo($request, 12)

    $Serial.Write($request, 0, $request.Length)
    $response = [byte[]]::new(8)
    $offset = 0
    while ($offset -lt $response.Length) {
        $offset += $Serial.Read($response, $offset, $response.Length - $offset)
    }
    $magic = [BitConverter]::ToUInt32($response, 0)
    $status = $response[4]
    $applied = $response[5]
    $sequence = [BitConverter]::ToUInt16($response, 6)
    $expected = if ($Active) { 1 } else { 0 }
    if ($magic -ne $hostStateResponseMagic -or $status -ne 0 -or
        $sequence -ne $script:hostSequence -or $applied -ne $expected) {
        throw ('Invalid host-activity acknowledgement: magic=0x{0:x8}, status={1}, active={2}, sequence={3}.' -f
            $magic, $status, $applied, $sequence)
    }
    $script:hostSequence = [uint16](($script:hostSequence + 1) -band 0xffff)
}

function Connect-Dongle {
    Disconnect-Dongle
    $candidates = @(Get-DongleCandidates)
    if ($candidates.Count -eq 0) {
        throw 'No ZMK USB serial dongle was found. Connect it or pass -Port COMxx.'
    }

    foreach ($candidate in $candidates) {
        $serial = [System.IO.Ports.SerialPort]::new($candidate, 115200, 'None', 8, 'One')
        $serial.ReadTimeout = 2500
        $serial.WriteTimeout = 2500
        $serial.DtrEnable = $true
        $serial.RtsEnable = $false
        try {
            $serial.Open()
            Start-Sleep -Milliseconds 500
            $serial.DiscardInBuffer()
            $serial.DiscardOutBuffer()
            Invoke-DongleState -Serial $serial -State 'Idle'
            $script:serial = $serial
            $script:selectedPort = $candidate
            return
        }
        catch {
            try {
                if ($serial.IsOpen) { $serial.Close() }
            } catch {}
            try { $serial.Dispose() } catch {}
            Write-Verbose "Rejected serial candidate $candidate`: $($_.Exception.Message)"
        }
    }
    throw 'No serial device answered the Merry Codex-state protocol.'
}

function Invoke-SelfTest {
    $vector = [Text.Encoding]::ASCII.GetBytes('123456789')
    $crc = [MerryCodexCrc32]::Compute($vector)
    if ($crc -ne [Convert]::ToUInt32('cbf43926', 16)) {
        throw ('CRC32 self-test failed: 0x{0:x8}' -f $crc)
    }
    if ($stateIds.Count -ne 5 -or $stateNames[$stateIds.Blocked] -ne 'Blocked') {
        throw 'State mapping self-test failed.'
    }
    if ($mediaBytes -ne 88880 -or $mediaStateIds.Playing -ne 1 -or
        $mediaStateIds.Paused -ne 2) {
        throw 'Media protocol self-test failed.'
    }
    if ($hostStateMagic -ne [uint32]0x3153484d -or $HostActivitySeconds -lt 5) {
        throw 'Host-activity protocol self-test failed.'
    }
    foreach ($codex in $stateNames) {
        foreach ($media in @('None', 'Playing', 'Paused')) {
            $expected = $media -ne 'None' -and $codex -ne 'Running'
            if ((Test-MerryMediaEligible -CodexState $codex -MediaState $media) -ne
                $expected) {
                throw "Priority self-test failed for Codex=$codex, Spotify=$media."
            }
        }
    }
    if (Test-MerryArtworkUploadNeeded -Eligible $true -MediaKey 'same-track' `
        -LastUploadedKey 'same-track' -WasEligible $true) {
        throw 'Pause/resume artwork self-test failed.'
    }
    if (-not (Test-MerryArtworkUploadNeeded -Eligible $true -MediaKey 'new-track' `
        -LastUploadedKey 'old-track' -WasEligible $true)) {
        throw 'Track-change artwork self-test failed.'
    }
    $throwingSerial = [pscustomobject]@{ IsOpen = $true }
    $throwingSerial | Add-Member -MemberType ScriptMethod -Name Close -Value {
        throw 'simulated missing USB device'
    }
    $throwingSerial | Add-Member -MemberType ScriptMethod -Name Dispose -Value {
        throw 'simulated disposed USB device'
    }
    $script:serial = $throwingSerial
    $script:selectedPort = 'COM_TEST'
    Disconnect-Dongle
    if ($null -ne $script:serial -or $null -ne $script:selectedPort) {
        throw 'Faulting serial-cleanup self-test failed.'
    }
    Write-Host 'Merry Codex bridge self-test passed.'
}

if ($SelfTest) {
    Invoke-SelfTest
    return
}

if ($TtlSeconds -le $PollSeconds + 3) {
    throw 'TtlSeconds must be at least four seconds longer than PollSeconds.'
}

$lastLive = @{}
$pulseState = $null
$pulseUntil = [DateTime]::MinValue
$state = 'Idle'
$lastReportedState = $null
$lastMediaState = 'None'
$lastMediaEligible = $false
$lastUploadedMediaKey = $null
$cachedMediaKey = $null
$cachedMediaBytes = $null
$mediaWarning = $null
$lastHostActive = $null
$lastGoodPlayback = [pscustomobject]@{ State = 'None'; Session = $null }
$missingSince = $null
$trackSnapshot = [pscustomobject]@{ Key = $null; Properties = $null }
$nextCodexPoll = [DateTime]::MinValue
$nextMetadataPoll = [DateTime]::MinValue
$nextHeartbeat = [DateTime]::MinValue
$serialRetryAt = [DateTime]::MinValue
$lastSentCodex = $null
$lastSentMedia = $null
$lastSentHost = $null

try {
    if (-not $DryRun) {
        Connect-Dongle
        Write-Host "Merry dongle connected on $script:selectedPort."
    }

    while ($true) {
        $now = [DateTime]::UtcNow

        $rawPlayback = [pscustomobject]@{ State = 'None'; Session = $null }
        if ($spotifyEnabled) {
            try {
                $rawPlayback = Get-MerrySpotifyPlayback
                $mediaWarning = $null
            }
            catch {
                if ($mediaWarning -ne $_.Exception.Message) {
                    Write-Warning "Spotify media session unavailable: $($_.Exception.Message)"
                    $mediaWarning = $_.Exception.Message
                }
            }
        }
        $fastPlaybackChanged = $rawPlayback.State -in @('Playing', 'Paused') -and
            $rawPlayback.State -ne $lastMediaState

        if ($now -ge $nextCodexPoll -and -not $fastPlaybackChanged) {
            try {
                if ($null -eq $script:pipe -or -not $script:pipe.IsConnected) {
                    Connect-CodexPipe
                    Write-Host 'Codex Desktop status pipe connected.'
                }
                $tasks = @(Get-CodexTasks)
                $attention = @($tasks | Where-Object status -In @(
                    'needs_attention', 'waitingOnApproval', 'waitingOnUserInput'
                ))
                $blocked = @($tasks | Where-Object status -In @(
                    'failed', 'error', 'systemError'
                ))
                $active = @($tasks | Where-Object status -EQ 'active')
                $currentLive = @{}
                foreach ($task in @($attention) + @($active)) {
                    $currentLive[$task.id] = $task.status
                }

                if ($blocked.Count -gt 0) {
                    $pulseState = 'Blocked'
                    $pulseUntil = $now.AddSeconds($FailureSeconds)
                    $state = 'Blocked'
                }
                elseif ($attention.Count -gt 0) {
                    $pulseState = $null
                    $state = 'NeedsInput'
                }
                elseif ($active.Count -gt 0) {
                    $pulseState = $null
                    $state = 'Running'
                }
                else {
                    $finished = @($lastLive.Keys | Where-Object {
                        -not $currentLive.ContainsKey($_)
                    })
                    if ($finished.Count -gt 0) {
                        $outcome = 'Completed'
                        foreach ($threadId in $finished) {
                            if ((Get-LastTurnOutcome -ThreadId $threadId) -eq 'Blocked') {
                                $outcome = 'Blocked'
                                break
                            }
                        }
                        $pulseState = $outcome
                        $pulseUntil = $now.AddSeconds(
                            $(if ($outcome -eq 'Blocked') {
                                $FailureSeconds
                            } else {
                                $CompletedSeconds
                            })
                        )
                    }
                    if ($null -ne $pulseState -and $now -lt $pulseUntil) {
                        $state = $pulseState
                    } else {
                        $pulseState = $null
                        $state = 'Idle'
                    }
                }
                $lastLive = $currentLive
            }
            catch {
                Write-Warning "Codex status unavailable: $($_.Exception.Message)"
                Disconnect-CodexPipe
                if ($Once) { throw }
            }
            $nextCodexPoll = [DateTime]::UtcNow.AddSeconds($PollSeconds)
        }

        $now = [DateTime]::UtcNow

        if ($rawPlayback.State -in @('Playing', 'Paused')) {
            $lastGoodPlayback = $rawPlayback
            $missingSince = $null
            $playback = $rawPlayback
        } else {
            if ($null -eq $missingSince -and
                $lastGoodPlayback.State -in @('Playing', 'Paused')) {
                $missingSince = $now
            }
            if ($null -ne $missingSince -and
                $now -lt $missingSince.AddSeconds($MediaMissingGraceSeconds)) {
                $playback = $lastGoodPlayback
            } else {
                $playback = [pscustomobject]@{ State = 'None'; Session = $null }
                $trackSnapshot = [pscustomobject]@{ Key = $null; Properties = $null }
            }
        }

        $playbackChanged = $playback.State -ne $lastMediaState
        if ($rawPlayback.State -in @('Playing', 'Paused') -and
            $now -ge $nextMetadataPoll -and -not $playbackChanged) {
            try {
                $freshTrack = Get-MerrySpotifyTrack -Playback $rawPlayback
                if ($null -ne $freshTrack.Key) {
                    $trackSnapshot = $freshTrack
                }
            }
            catch {
                if ($mediaWarning -ne $_.Exception.Message) {
                    Write-Warning "Spotify metadata unavailable: $($_.Exception.Message)"
                    $mediaWarning = $_.Exception.Message
                }
            }
            $nextMetadataPoll = [DateTime]::UtcNow.AddSeconds(1)
        }

        $snapshot = [pscustomobject]@{
            State = $playback.State
            Key = $trackSnapshot.Key
            Properties = $trackSnapshot.Properties
        }
        $mediaEligible = Test-MerryMediaEligible -CodexState $state `
            -MediaState $snapshot.State
        $hostActive = $state -ne 'Idle' -or $snapshot.State -eq 'Playing'
        if ($windowsDesktop -and -not $hostActive) {
            $hostActive = [MerryHostActivity]::IdleMilliseconds() -le
                [uint32]($HostActivitySeconds * 1000)
        }

        if ($state -ne $lastReportedState) {
            Write-Host ('{0:HH:mm:ss.fff}  Codex pet: {1}' -f [DateTime]::Now, $state)
            $lastReportedState = $state
        }
        if ($snapshot.State -ne $lastMediaState) {
            Write-Host ('{0:HH:mm:ss.fff}  Spotify: {1}' -f [DateTime]::Now, $snapshot.State)
            $lastMediaState = $snapshot.State
        }
        if ($hostActive -ne $lastHostActive) {
            Write-Host ('{0:HH:mm:ss.fff}  Host activity: {1}' -f [DateTime]::Now,
                $(if ($hostActive) { 'Active' } else { 'AFK' }))
            $lastHostActive = $hostActive
        }

        if (-not $DryRun -and $now -ge $serialRetryAt) {
            try {
                if ($null -eq $script:serial -or -not $script:serial.IsOpen) {
                    Connect-Dongle
                    Write-Host "Merry dongle reconnected on $script:selectedPort."
                    $lastSentCodex = $null
                    $lastSentMedia = $null
                    $lastSentHost = $null
                    $lastMediaEligible = $false
                    $lastUploadedMediaKey = $null
                }
                $heartbeatDue = $now -ge $nextHeartbeat
                if ($heartbeatDue -or $state -ne $lastSentCodex) {
                    Invoke-DongleState -Serial $script:serial -State $state
                    $lastSentCodex = $state
                }
                if (Test-MerryArtworkUploadNeeded -Eligible $mediaEligible `
                    -MediaKey $snapshot.Key -LastUploadedKey $lastUploadedMediaKey `
                    -WasEligible $lastMediaEligible) {
                    if ($snapshot.Key -ne $cachedMediaKey -or $null -eq $cachedMediaBytes) {
                        $cachedMediaBytes = ConvertTo-MerryAlbumBytes -Snapshot $snapshot
                        $cachedMediaKey = $snapshot.Key
                    }
                    Write-Host ('{0:HH:mm:ss.fff}  Uploading Spotify album artwork...' -f
                        [DateTime]::Now)
                    Invoke-DongleMediaUpload -Serial $script:serial -Image $cachedMediaBytes `
                        -State $snapshot.State
                    $lastUploadedMediaKey = $snapshot.Key
                    Write-Host ('{0:HH:mm:ss.fff}  Spotify album artwork ready.' -f
                        [DateTime]::Now)
                }
                if ($heartbeatDue -or $snapshot.State -ne $lastSentMedia) {
                    Invoke-DongleMediaState -Serial $script:serial -State $snapshot.State
                    $lastSentMedia = $snapshot.State
                }
                if ($heartbeatDue -or $hostActive -ne $lastSentHost) {
                    Invoke-DongleHostActivity -Serial $script:serial -Active $hostActive
                    $lastSentHost = $hostActive
                }
                if ($heartbeatDue) {
                    $nextHeartbeat = [DateTime]::UtcNow.AddSeconds($PollSeconds)
                }
                $lastMediaEligible = $mediaEligible
            }
            catch {
                Write-Warning "Dongle serial unavailable: $($_.Exception.Message)"
                Disconnect-Dongle
                $serialRetryAt = [DateTime]::UtcNow.AddSeconds(2)
                if ($Once) { throw }
            }
        } else {
            $lastMediaEligible = $mediaEligible
        }

        if ($Once) { break }
        Start-Sleep -Milliseconds $MediaPollMilliseconds
    }
}
finally {
    Disconnect-CodexPipe
    Disconnect-Dongle
}
