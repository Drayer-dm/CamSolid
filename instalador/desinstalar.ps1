# Desinstala CamSolid: quita la cámara de Windows, los archivos y los accesos directos.
param([switch]$SinPausa)
$ErrorActionPreference = 'Continue'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argumentos = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($SinPausa) { $argumentos += '-SinPausa' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argumentos
    exit
}

# Registro de la desinstalación, para diagnosticar problemas
Start-Transcript -Path (Join-Path $env:TEMP 'CamSolid-desinstalar.log') -Force | Out-Null

$destino = Join-Path $env:ProgramFiles 'CamSolid'
Write-Host "Desinstalando CamSolid de $destino"

# 1. Cerrar CamSolid y quitar la redirección de adb
$camsolid = Join-Path $destino 'CamSolid.exe'
if (Test-Path $camsolid) { & $camsolid --stop | Out-Null }
Get-Process CamSolid -ErrorAction SilentlyContinue | ForEach-Object { $_.WaitForExit(10000) | Out-Null }
$adb = Join-Path $destino 'adb.exe'
if (Test-Path $adb) {
    & $adb forward --remove tcp:27183 2>$null | Out-Null
    & $adb kill-server 2>$null | Out-Null
}

# 2. Quitar la cámara de Windows. Hay que detener el servicio de cámaras para liberar la DLL.
$dll = Join-Path $destino 'CamSolidSource.dll'
if (Test-Path $dll) {
    Start-Process "$env:WINDIR\System32\regsvr32.exe" -ArgumentList '/u', '/s', "`"$dll`"" -Wait | Out-Null
}
Stop-Service FrameServerMonitor -Force -ErrorAction SilentlyContinue
Stop-Service FrameServer -Force -ErrorAction SilentlyContinue

# 3. Borrar archivos, accesos directos y la entrada de "Agregar o quitar programas".
#    Este script está en la carpeta que se borra: se copia antes a una carpeta temporal.
Remove-Item "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\CamSolid.lnk", "$env:PUBLIC\Desktop\CamSolid.lnk" -Force -ErrorAction SilentlyContinue
Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CamSolid' -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem $destino -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Set-Location $env:TEMP
Remove-Item $destino -Recurse -Force -ErrorAction SilentlyContinue

Start-Service FrameServer -ErrorAction SilentlyContinue

Write-Host ''
if (Test-Path $destino) {
    Write-Host "CamSolid se desinstaló, pero no se pudo borrar $destino del todo. Puedes borrarla a mano."
} else {
    Write-Host 'CamSolid se desinstaló.'
}
Stop-Transcript | Out-Null
if (-not $SinPausa) { Read-Host 'Pulsa Enter para cerrar' }
