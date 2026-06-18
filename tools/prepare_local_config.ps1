$ErrorActionPreference = "Stop"

$configPath = ".\tools\stt_config.json"
$examplePath = ".\tools\stt_config.example.json"

if (-not (Test-Path -LiteralPath $examplePath)) {
  throw "Example config not found: $examplePath"
}

if (-not (Test-Path -LiteralPath $configPath)) {
  Copy-Item -LiteralPath $examplePath -Destination $configPath
  Write-Host "Created $configPath from example. Fill api_key and image_api_key before syncing."
  exit 0
}

$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
$example = Get-Content -LiteralPath $examplePath -Raw | ConvertFrom-Json

foreach ($property in $example.PSObject.Properties) {
  if ($null -eq $config.PSObject.Properties[$property.Name]) {
    $config | Add-Member -NotePropertyName $property.Name -NotePropertyValue $property.Value
  }
}

if ($config.image_model -eq "wan2.6-t2i") {
  $config.image_model = $example.image_model
}

if ($config.image_size -eq "1280*1280") {
  $config.image_size = $example.image_size
}

if ($config.image_model -eq $example.image_model -and $config.image_generation_url -like "*multimodal-generation/generation") {
  $config.image_generation_url = $example.image_generation_url
}

$config |
  ConvertTo-Json -Depth 10 |
  Set-Content -LiteralPath $configPath -Encoding utf8

Write-Host "Updated $configPath with any missing fields."
Write-Host "Now fill image_api_key locally, then run: .\tools\sync_orangepi_config.ps1"
