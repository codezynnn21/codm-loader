#requires -Version 5.1
param(
    [ValidateSet('setup', 'server', 'recon', 'patch', 'pull', 'stop')]
    [string]$Action = 'setup',
    [string]$Package = 'com.activision.callofduty.shooter',
    [string]$Remote = '/data/local/tmp'
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Script = Join-Path $Root 'codm.js'
$Config = Join-Path $Root 'config.json'
$ReconOut = Join-Path $Root 'codm_recon.json'
$RemoteRecon = "$Remote/codm_recon.json"

function Assert-Adb {
    if (-not (Get-Command adb -ErrorAction SilentlyContinue)) {
        throw 'adb not on PATH - install platform-tools and add it to PATH'
    }
    $devices = (& adb devices) -split "`n" | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' }
    if (-not $devices) { throw 'no device - check usb debugging or emulator adb bridge' }
}

function Push-Files {
    if (-not (Test-Path $Script)) { throw "missing $Script" }
    if (-not (Test-Path $Config)) { throw "missing $Config" }
    & adb push $Script "$Remote/codm.js" | Out-Null
    & adb push $Config "$Remote/codm_config.json" | Out-Null
    Write-Host "[push] codm.js + config.json -> $Remote"
}

function Start-Server {
    $local = Join-Path $Root 'frida-server'
    if (Test-Path $local) {
        & adb push $local "$Remote/frida-server" | Out-Null
        Write-Host "[server] pushed frida-server"
    }
    & adb shell "su -c 'pkill -f frida-server'" 2>$null | Out-Null
    & adb shell "su -c 'chmod 755 $Remote/frida-server; nohup $Remote/frida-server >/dev/null 2>&1 &'" | Out-Null
    Start-Sleep -Seconds 2
    $pid = (& adb shell "su -c 'pidof frida-server'") -join ''
    if (-not $pid.Trim()) { throw 'frida-server did not start - root required' }
    Write-Host "[server] frida-server pid $($pid.Trim())"
}

function Assert-Mode([string]$expected) {
    $cfg = Get-Content $Config -Raw | ConvertFrom-Json
    if ($cfg.mode -ne $expected) {
        Write-Warning "config.json mode is '$($cfg.mode)', expected '$expected' - edit it first"
    }
}

function Assert-Frida {
    if (-not (Get-Command frida -ErrorAction SilentlyContinue)) {
        throw 'frida CLI not on PATH - pip install frida-tools'
    }
    $hostVersion = (& frida --version) -join ''
    Write-Host "[frida] host $hostVersion - device server must match exactly"
}

function Invoke-Recon {
    Assert-Mode 'recon'
    Push-Files
    & adb shell "rm -f $RemoteRecon" | Out-Null
    $proc = Start-Process -FilePath 'frida' -PassThru -ArgumentList @(
        '-U', '-f', $Package, '-l', $Script, '-q'
    )
    Write-Host "[recon] spawned $Package, waiting for il2cpp + scan (up to 120s)"
    $found = $false
    for ($i = 0; $i -lt 120; $i++) {
        Start-Sleep -Seconds 1
        & adb shell "test -f $RemoteRecon" | Out-Null
        if ($LASTEXITCODE -eq 0) { $found = $true; break }
        if ($proc.HasExited) { break }
    }
    if ($found) {
        & adb pull $RemoteRecon $ReconOut | Out-Null
        Write-Host "[recon] saved $ReconOut"
    } else {
        Write-Warning 'recon file never appeared - app may not have reached the menu; check frida output'
    }
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}

function Invoke-Patch {
    Assert-Mode 'patch'
    Push-Files
    Write-Host "[patch] spawning $Package - close the frida session to detach"
    & frida -U -f $Package -l $Script
}

Assert-Adb
Assert-Frida

switch ($Action) {
    'setup' { Start-Server; Push-Files }
    'server' { Start-Server }
    'recon' { Invoke-Recon }
    'patch' { Invoke-Patch }
    'pull' { & adb pull $RemoteRecon $ReconOut; Write-Host "[pull] $ReconOut" }
    'stop' {
        & adb shell "su -c 'pkill -f frida-server'" 2>$null | Out-Null
        Get-Process frida -ErrorAction SilentlyContinue | Stop-Process -Force
        Write-Host '[stop] frida-server and host sessions killed'
    }
}
