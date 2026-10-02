$ErrorActionPreference = "Stop"

$arduinoCli = "D:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = "E:\esp32\build_smartwatch_huge"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

& $arduinoCli compile `
  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app" `
  --libraries "$projectDir\libraries" `
  --build-path $buildDir `
  $projectDir
