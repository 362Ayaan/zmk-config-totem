[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\artifacts\MerryDongle-Windows')
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = [IO.Path]::GetFullPath($OutputDirectory)
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$automation = Get-ChildItem "$env:WINDIR\Microsoft.NET\assembly\GAC_MSIL\System.Management.Automation" `
    -Filter System.Management.Automation.dll -Recurse | Select-Object -First 1
if (-not (Test-Path $csc) -or $null -eq $automation) {
    throw 'Windows PowerShell 5.1/.NET Framework compiler is unavailable.'
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $output 'tools') -Force | Out-Null

& $csc /nologo /target:winexe /optimize+ /platform:x64 `
    "/out:$output\MerryHost.exe" `
    "/reference:$($automation.FullName)" `
    /reference:System.Runtime.Serialization.dll `
    (Join-Path $PSScriptRoot 'MerryHost\Host.cs')
if ($LASTEXITCODE -ne 0) { throw 'MerryHost compilation failed.' }

& $csc /nologo /target:exe /optimize+ /platform:x64 `
    "/out:$output\MerryControl.exe" `
    /reference:System.Runtime.Serialization.dll `
    (Join-Path $PSScriptRoot 'MerryControl\Program.cs')
if ($LASTEXITCODE -ne 0) { throw 'MerryControl compilation failed.' }

Copy-Item (Join-Path $root 'tools\merry_codex_bridge.ps1') (Join-Path $output 'tools') -Force
Copy-Item (Join-Path $root 'tools\merry_media_helpers.ps1') (Join-Path $output 'tools') -Force

$default = '{"Mode":"Auto","Brightness":100,"ScreenOffSeconds":300,"Port":"Auto"}'
[IO.File]::WriteAllText((Join-Path $output 'MerryConfig.json'), $default,
                        [Text.UTF8Encoding]::new($false))
$checksumPath = Join-Path $output 'SHA256SUMS.txt'
Get-ChildItem $output -Recurse -File | Where-Object { $_.FullName -ne $checksumPath } |
    Get-FileHash -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash, $_.Path.Substring($output.Length + 1) } |
    Set-Content $checksumPath -Encoding ASCII

Write-Host "Built Merry Windows package at $output"
