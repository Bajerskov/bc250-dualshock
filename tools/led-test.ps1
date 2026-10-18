<#
.SYNOPSIS
  Test and configure the BC-250 LED bar from PowerShell.

.DESCRIPTION
  Drives the strip two ways:
    - HTTP to the ESP32's /api/led/* routes, for config and the built-in pattern
    - UDP DRGB on port 21324, the same protocol the SteamOS bridge uses

  Streamed modes send frames continuously, because the firmware reverts to its
  idle animation about a second after packets stop.

.PARAMETER Mode
  status     show live config and stream state
  test       built-in red/green/blue pattern (1 s each, via HTTP)
  solid      hold one colour         -R -G -B [-Seconds]
  chase      walk one lit LED down the strip, twice  [-Seconds]
  rainbow    moving rainbow          [-Seconds]
  gradient   static black-to-white ramp, checks banding
  off        all LEDs black
  config     set strip settings      [-Leds] [-Brightness] [-Order]

.EXAMPLE
  .\tools\led-test.ps1 status
.EXAMPLE
  .\tools\led-test.ps1 chase
.EXAMPLE
  .\tools\led-test.ps1 solid -R 255 -G 0 -B 0
.EXAMPLE
  .\tools\led-test.ps1 config -Leds 24 -Brightness 128 -Order RGB
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('status', 'test', 'solid', 'chase', 'rainbow', 'gradient', 'off', 'config')]
    [string]$Mode = 'status',

    # NOTE: deliberately not named -Host; $Host is a reserved PowerShell variable.
    [string]$Ip = '192.168.4.1',
    [int]$Leds = 0,                  # 0 = read the configured count from the device
    [int]$Seconds = 5,
    [ValidateRange(0, 255)][int]$R = 255,
    [ValidateRange(0, 255)][int]$G = 255,
    [ValidateRange(0, 255)][int]$B = 255,
    [ValidateRange(1, 255)][int]$Brightness = 0,
    [ValidateSet('GRB', 'RGB', 'BRG', 'RBG', 'GBR', 'BGR')][string]$Order
)

$ErrorActionPreference = 'Stop'
$UDP_PORT = 21324
$DRGB     = 2
$TIMEOUT  = 2          # seconds, byte 1 of the packet
$FPS      = 30
$MAX_LEDS = 24

function Get-LedConfig {
    try {
        Invoke-RestMethod -Uri "http://$Ip/api/led/config" -TimeoutSec 10
    } catch {
        throw "Cannot reach $Ip - is this machine on the same network as the ESP32? ($($_.Exception.Message))"
    }
}

function Resolve-LedCount {
    if ($Leds -gt 0) { return [Math]::Min($Leds, $MAX_LEDS) }
    $c = (Get-LedConfig).ledCount
    if (-not $c) { return 8 }
    return [Math]::Min([int]$c, $MAX_LEDS)
}

# Build a DRGB packet from an array of @(r,g,b) triples.
function New-DrgbPacket([object[]]$Pixels) {
    $buf = New-Object byte[] (2 + $Pixels.Count * 3)
    $buf[0] = $DRGB
    $buf[1] = $TIMEOUT
    for ($i = 0; $i -lt $Pixels.Count; $i++) {
        $o = 2 + $i * 3
        $buf[$o]     = [byte]$Pixels[$i][0]
        $buf[$o + 1] = [byte]$Pixels[$i][1]
        $buf[$o + 2] = [byte]$Pixels[$i][2]
    }
    return $buf
}

# Stream frames until $Seconds elapses. $FrameFn takes the frame index and
# returns an array of @(r,g,b) triples.
function Send-Stream([int]$Count, [scriptblock]$FrameFn, [int]$Duration) {
    $udp = New-Object System.Net.Sockets.UdpClient
    try {
        $udp.Connect($Ip, $UDP_PORT)
        $delay = [int](1000 / $FPS)
        $deadline = (Get-Date).AddSeconds($Duration)
        $frame = 0
        while ((Get-Date) -lt $deadline) {
            $pixels = & $FrameFn $frame $Count
            $pkt = New-DrgbPacket $pixels
            [void]$udp.Send($pkt, $pkt.Length)
            $frame++
            Start-Sleep -Milliseconds $delay
        }
        Write-Host "  sent $frame frames of $($pkt.Length) bytes to ${Ip}:$UDP_PORT"
    } finally {
        $udp.Close()
    }
}

function ConvertFrom-Hsv([double]$h, [double]$s, [double]$v) {
    $i = [Math]::Floor($h * 6)
    $f = $h * 6 - $i
    $p = $v * (1 - $s); $q = $v * (1 - $f * $s); $t = $v * (1 - (1 - $f) * $s)
    switch ($i % 6) {
        0 { $r = $v; $g = $t; $b = $p }
        1 { $r = $q; $g = $v; $b = $p }
        2 { $r = $p; $g = $v; $b = $t }
        3 { $r = $p; $g = $q; $b = $v }
        4 { $r = $t; $g = $p; $b = $v }
        5 { $r = $v; $g = $p; $b = $q }
    }
    return @([int]($r * 255), [int]($g * 255), [int]($b * 255))
}

switch ($Mode) {

    'status' {
        $c = Get-LedConfig
        Write-Host "LED bar at $Ip" -ForegroundColor Cyan
        Write-Host ("  enabled        : {0}" -f $c.enabled)
        Write-Host ("  ledCount       : {0}  (max {1})" -f $c.ledCount, $c.maxLedCount)
        Write-Host ("  maxBrightness  : {0}" -f $c.maxBrightness)
        Write-Host ("  colorOrder     : {0}" -f $c.colorOrder)
        Write-Host ("  idleAnimations : {0}" -f $c.idleAnimations)
        Write-Host ("  streaming      : {0}" -f $c.streaming)
        $age = [int]$c.lastPacketAge
        Write-Host ("  lastPacketAge  : {0}" -f $(if ($age -lt 0) { 'never' } else { "$age ms" }))
        Write-Host ("  droppedPackets : {0}" -f $c.droppedPackets)
        try {
            $s = Invoke-RestMethod -Uri "http://$Ip/api/status" -TimeoutSec 10
            Write-Host ("  firmware       : v{0}   PC: {1}" -f $s.version, $(if ($s.pcOn) { 'ON' } else { 'OFF' }))
        } catch { }
    }

    'test' {
        Write-Host "Built-in pattern: red, green, blue - 1 s each. Watch the strip." -ForegroundColor Cyan
        Invoke-RestMethod -Uri "http://$Ip/api/led/test" -Method Post -TimeoutSec 10 | Out-Null
        Start-Sleep -Seconds 3
        Write-Host "  done. The order you saw IS your strip's order only if colorOrder is set to RGB." -ForegroundColor Yellow
    }

    'solid' {
        $n = Resolve-LedCount
        Write-Host "Solid ($R,$G,$B) on $n LEDs for ${Seconds}s" -ForegroundColor Cyan
        Send-Stream $n { param($f, $c) @(, @($R, $G, $B)) * $c } $Seconds
    }

    'chase' {
        $n = Resolve-LedCount
        $dur = [Math]::Max($Seconds, [int][Math]::Ceiling($n * 2 * 0.18) + 1)
        Write-Host "Chase across $n LEDs, twice. Count how many light up, and in which direction." -ForegroundColor Cyan
        $udp = New-Object System.Net.Sockets.UdpClient
        try {
            $udp.Connect($Ip, $UDP_PORT)
            for ($pass = 1; $pass -le 2; $pass++) {
                for ($i = 0; $i -lt $n; $i++) {
                    $px = @()
                    for ($j = 0; $j -lt $n; $j++) {
                        if ($j -eq $i) { $px += , @(255, 255, 255) } else { $px += , @(0, 0, 0) }
                    }
                    $pkt = New-DrgbPacket $px
                    # resend so the stream cannot time out between steps
                    [void]$udp.Send($pkt, $pkt.Length)
                    Start-Sleep -Milliseconds 60
                    [void]$udp.Send($pkt, $pkt.Length)
                    Start-Sleep -Milliseconds 120
                    Write-Host ("`r  LED {0,2} of {1}" -f ($i + 1), $n) -NoNewline
                }
            }
            Write-Host ""
        } finally { $udp.Close() }
        Write-Host "  If fewer than $n lit, the count or the wiring is off." -ForegroundColor Yellow
    }

    'rainbow' {
        $n = Resolve-LedCount
        Write-Host "Moving rainbow on $n LEDs for ${Seconds}s - look for flicker or stutter" -ForegroundColor Cyan
        Send-Stream $n {
            param($f, $c)
            $out = @()
            for ($i = 0; $i -lt $c; $i++) {
                $h = ((($f * 0.01) + ($i / [double]$c)) % 1.0)
                $out += , (ConvertFrom-Hsv $h 1.0 1.0)
            }
            return $out
        } $Seconds
    }

    'gradient' {
        $n = Resolve-LedCount
        Write-Host "Static black-to-white ramp on $n LEDs for ${Seconds}s - look for banding" -ForegroundColor Cyan
        Send-Stream $n {
            param($f, $c)
            $out = @()
            for ($i = 0; $i -lt $c; $i++) {
                $v = [int](255 * $i / [Math]::Max(1, $c - 1))
                $out += , @($v, $v, $v)
            }
            return $out
        } $Seconds
    }

    'off' {
        $n = Resolve-LedCount
        Write-Host "All $n LEDs black for 2 s, then the idle animation resumes" -ForegroundColor Cyan
        Send-Stream $n { param($f, $c) @(, @(0, 0, 0)) * $c } 2
    }

    'config' {
        $body = @{}
        if ($Leds -gt 0)       { $body.ledCount      = [Math]::Min($Leds, $MAX_LEDS) }
        if ($Brightness -gt 0) { $body.maxBrightness = $Brightness }
        if ($Order)            { $body.colorOrder    = $Order }
        if ($body.Count -eq 0) {
            Write-Host "Nothing to set. Use -Leds, -Brightness and/or -Order." -ForegroundColor Yellow
            return
        }
        $json = $body | ConvertTo-Json -Compress
        Write-Host "POST $json" -ForegroundColor Cyan
        try {
            Invoke-RestMethod -Uri "http://$Ip/api/led/config" -Method Post `
                -ContentType 'application/json' -Body $json -TimeoutSec 15 | Out-Null
        } catch {
            throw "Rejected: $($_.Exception.Message). ledCount must be 1-24, maxBrightness 1-255."
        }
        $c = Get-LedConfig
        Write-Host ("  now: {0} LEDs, brightness {1}, order {2}" -f $c.ledCount, $c.maxBrightness, $c.colorOrder) -ForegroundColor Green
    }
}
