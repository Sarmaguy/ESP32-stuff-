# Logs the master's MQ135 reading to CSV so the air-quality thresholds can be set
# from a real distribution instead of two spot readings. Leave it running for a
# few days, annotating the "note" column when you open/close windows.
#
#   .\aq_log.ps1
#   .\aq_log.ps1 -IntervalSec 30 -Path D:\aq.csv
#
# Temp and humidity are logged alongside because both raise the MQ135 reading on
# their own - a baseline taken at 20C is not comparable to one taken at 27C.

param(
  [string]$DeviceIp   = "192.168.0.176",
  [int]   $IntervalSec = 60,
  [string]$Path       = "$PSScriptRoot\aq_log.csv"
)

if (-not (Test-Path $Path)) {
  "timestamp,aq,voltage,temp,humidity,label,note" | Out-File -FilePath $Path -Encoding utf8
}

Write-Host "Logging $DeviceIp -> $Path every ${IntervalSec}s. Ctrl+C to stop."

while ($true) {
  try {
    $r = Invoke-RestMethod -Uri "http://$DeviceIp/api/status" -TimeoutSec 8
    if ($r.aqReady) {
      $line = "{0},{1},{2:F3},{3:F1},{4:F1},{5}," -f `
        (Get-Date -Format "s"), $r.aq, $r.aqV, $r.temp, $r.hum, $r.aqLabel
      Add-Content -Path $Path -Value $line -Encoding utf8
      Write-Host ("{0}  raw={1,4}  {2,-9}  {3:F1}C  {4:F0}%" -f `
        (Get-Date -Format "HH:mm:ss"), $r.aq, $r.aqLabel, $r.temp, $r.hum)
    } else {
      # Readings during the 3-minute warm-up are the warm-up curve, not the room.
      Write-Host ("{0}  warming up ({1}s left)" -f (Get-Date -Format "HH:mm:ss"), $r.aqWarm)
    }
  } catch {
    # This LAN drops 3-6% of requests; a missed sample is normal, not a failure.
    Write-Host ("{0}  (no response)" -f (Get-Date -Format "HH:mm:ss")) -ForegroundColor DarkGray
  }
  Start-Sleep -Seconds $IntervalSec
}
