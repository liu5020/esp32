param(
  [string]$HostName = "192.168.31.58",
  [string]$UserName = "HwHiAiUser",
  [string]$AppDir = "~/voice_ai_mic_server",
  [string]$ConfigPath = ".\tools\stt_config.json"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ConfigPath)) {
  throw "Config file not found: $ConfigPath. Copy tools\stt_config.example.json to tools\stt_config.json first."
}

$remoteConfig = "${UserName}@${HostName}:$AppDir/tools/stt_config.json"
$remoteShell = "${UserName}@${HostName}"

Write-Host "Uploading $ConfigPath to $remoteConfig"
& scp $ConfigPath $remoteConfig
if ($LASTEXITCODE -ne 0) {
  throw "scp failed with exit code $LASTEXITCODE"
}

Write-Host "Restarting Orange Pi bridge"
& ssh $remoteShell "cd $AppDir && ./tools/orangepi_bridge.sh restart && ./tools/orangepi_bridge.sh status"
if ($LASTEXITCODE -ne 0) {
  throw "ssh restart failed with exit code $LASTEXITCODE"
}

Write-Host "Done. Test: http://$HostName`:8787/health"
