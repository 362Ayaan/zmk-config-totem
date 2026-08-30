[CmdletBinding()]
param(
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [ValidateRange(1, 30)]
    [int]$PollSeconds = 2,

    [ValidateRange(5, 60)]
    [int]$TtlSeconds = 12,

    [ValidateRange(1, 60)]
    [int]$CompletedSeconds = 8,

    [ValidateRange(1, 120)]
    [int]$FailureSeconds = 20,

    [switch]$DryRun,
    [switch]$Once,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

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
$script:requestId = 0
$script:sequence = [uint16]0
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
    if ($null -ne $script:pipe) {
        $script:pipe.Dispose()
        $script:pipe = $null
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
    $response = [Text.Encoding]::UTF8.GetString($responseBytes) | ConvertFrom-Json -Depth 60
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
    return $text.text | ConvertFrom-Json -Depth 80
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
    if ($null -ne $script:serial) {
        if ($script:serial.IsOpen) {
            $script:serial.Close()
        }
        $script:serial.Dispose()
        $script:serial = $null
    }
    $script:selectedPort = $null
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

function Connect-Dongle {
    Disconnect-Dongle
    $candidates = Get-DongleCandidates
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
            if ($serial.IsOpen) {
                $serial.Close()
            }
            $serial.Dispose()
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
$lastReportedState = $null

try {
    if (-not $DryRun) {
        Connect-Dongle
        Write-Host "Merry dongle connected on $script:selectedPort."
    }

    while ($true) {
        $phase = 'codex'
        try {
            if ($null -eq $script:pipe -or -not $script:pipe.IsConnected) {
                Connect-CodexPipe
                Write-Host 'Codex Desktop status pipe connected.'
            }

            $tasks = @(Get-CodexTasks)
            $attention = @($tasks | Where-Object status -In @(
                'needs_attention', 'waitingOnApproval', 'waitingOnUserInput'
            ))
            $blocked = @($tasks | Where-Object status -In @('failed', 'error', 'systemError'))
            $active = @($tasks | Where-Object status -EQ 'active')
            $currentLive = @{}
            foreach ($task in @($attention) + @($active)) {
                $currentLive[$task.id] = $task.status
            }

            if ($blocked.Count -gt 0) {
                $pulseState = 'Blocked'
                $pulseUntil = [DateTime]::UtcNow.AddSeconds($FailureSeconds)
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
                $finished = @($lastLive.Keys | Where-Object { -not $currentLive.ContainsKey($_) })
                if ($finished.Count -gt 0) {
                    $outcome = 'Completed'
                    foreach ($threadId in $finished) {
                        if ((Get-LastTurnOutcome -ThreadId $threadId) -eq 'Blocked') {
                            $outcome = 'Blocked'
                            break
                        }
                    }
                    $pulseState = $outcome
                    $pulseUntil = [DateTime]::UtcNow.AddSeconds(
                        $(if ($outcome -eq 'Blocked') { $FailureSeconds } else { $CompletedSeconds })
                    )
                }
                if ($null -ne $pulseState -and [DateTime]::UtcNow -lt $pulseUntil) {
                    $state = $pulseState
                }
                else {
                    $pulseState = $null
                    $state = 'Idle'
                }
            }
            $lastLive = $currentLive

            if ($state -ne $lastReportedState) {
                Write-Host ('{0:HH:mm:ss}  Codex pet: {1}' -f [DateTime]::Now, $state)
                $lastReportedState = $state
            }
            if (-not $DryRun) {
                $phase = 'serial'
                if ($null -eq $script:serial -or -not $script:serial.IsOpen) {
                    Connect-Dongle
                    Write-Host "Merry dongle reconnected on $script:selectedPort."
                }
                Invoke-DongleState -Serial $script:serial -State $state
            }

            if ($Once) {
                break
            }
            Start-Sleep -Seconds $PollSeconds
        }
        catch {
            Write-Warning $_.Exception.Message
            if ($phase -eq 'codex') {
                Disconnect-CodexPipe
            }
            elseif (-not $DryRun) {
                Disconnect-Dongle
            }
            if ($Once) {
                throw
            }
            Start-Sleep -Seconds ([Math]::Max(2, $PollSeconds))
        }
    }
}
finally {
    Disconnect-CodexPipe
    Disconnect-Dongle
}
