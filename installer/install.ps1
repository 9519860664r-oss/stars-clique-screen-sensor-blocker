# Stars Clique Screen Sensor Blocker — install / uninstall.
# Copies the program to Program Files and starts it at logon with admin rights
# (via Task Scheduler, so there is no UAC prompt on every boot).
#
#   install.ps1 -Lang ru|en              install (or update)
#   install.ps1 -Lang ru|en -Uninstall   remove
param([ValidateSet('ru', 'en')][string]$Lang = 'en', [switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$name = 'Stars Clique Screen Sensor Blocker'
$dir  = Join-Path $env:ProgramFiles 'Stars Clique\Screen Sensor Blocker'
$exe  = Join-Path $dir "$name.exe"
$task = $name

function T([string]$ru, [string]$en) { if ($Lang -eq 'ru') { $ru } else { $en } }

# Re-run elevated if needed.
$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $a = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Lang $Lang"
    if ($Uninstall) { $a += ' -Uninstall' }
    Start-Process powershell.exe $a -Verb RunAs -Wait
    exit
}

# The program sits next to this script in a release zip, or in bin\<lang> in the source tree.
$src = @((Join-Path $PSScriptRoot "$name.exe"), (Join-Path $PSScriptRoot "..\bin\$Lang\$name.exe")) |
       Where-Object { Test-Path $_ } | Select-Object -First 1

# Stop a running copy and remove the old autostart entry.
Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
Start-Sleep -Milliseconds 500
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue

if ($Uninstall) {
    # Make sure the touch screen is left on.
    Get-PnpDevice -Class HIDClass | Where-Object {
        (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_HardwareIds -ErrorAction SilentlyContinue).Data -contains 'HID_DEVICE_UP:000D_U:0004'
    } | Where-Object Status -ne 'OK' | Enable-PnpDevice -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\Stars Clique\Screen Sensor Blocker' -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host (T "$name удалён." "$name has been removed.") -ForegroundColor Green
    Start-Sleep 2
    exit
}

if (-not $src) {
    Write-Host (T "Не найден файл `"$name.exe`" рядом с установщиком." "`"$name.exe`" was not found next to the installer.") -ForegroundColor Red
    Start-Sleep 5
    exit 1
}

New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $src $exe -Force

$user      = "$env:USERDOMAIN\$env:USERNAME"
$action    = New-ScheduledTaskAction -Execute $exe
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User $user
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
             -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -Priority 4
Register-ScheduledTask -TaskName $task -Action $action -Trigger $trigger -Principal $principal -Settings $settings `
    -Description 'Turns the touch screen off / on with a hotkey.' -Force | Out-Null
Start-ScheduledTask -TaskName $task

Write-Host (T "$name установлен и запущен." "$name is installed and running.") -ForegroundColor Green
$hint = T "Num * (на ноутбуках часто Fn + *) — выключить / включить сенсор. Правый клик по значку в трее — настройки." `
          "Num * (often Fn + * on laptops) turns the touch screen off / on. Right-click the tray icon for settings."
Write-Host $hint
Start-Sleep 4
