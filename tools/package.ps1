# Builds both versions and packs the release zips into dist\.
# Usage: tools\package.ps1 -Version 1.0.0
param([string]$Version = '1.0.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$name = 'Stars Clique Screen Sensor Blocker'

& "$env:SystemRoot\System32\cmd.exe" /c "`"$root\src\build.bat`""
if ($LASTEXITCODE) { throw 'build failed' }

$dist = Join-Path $root 'dist'
Remove-Item $dist -Recurse -Force -ErrorAction SilentlyContinue
foreach ($lang in 'ru', 'en') {
    $stage = Join-Path $dist "stage-$lang\$name"
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item "$root\bin\$lang\$name.exe", "$root\installer\install.ps1", "$root\LICENSE" $stage
    if ($lang -eq 'ru') { Copy-Item "$root\installer\Установить.cmd", "$root\installer\Удалить.cmd" $stage }
    else                { Copy-Item "$root\installer\Install.cmd", "$root\installer\Uninstall.cmd" $stage }
    $zip = Join-Path $dist ("StarsClique-ScreenSensorBlocker-v$Version-{0}.zip" -f $lang.ToUpper())
    Compress-Archive -Path $stage -DestinationPath $zip
    Remove-Item (Join-Path $dist "stage-$lang") -Recurse -Force
}
Get-ChildItem $dist | Select-Object Name, Length
