# Instala CamSolid en este PC. Se ejecuta con Instalar.cmd, que lo abre con
# permisos de administrador: registrar la cámara en Windows los necesita.
param([switch]$SinPausa)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argumentos = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($SinPausa) { $argumentos += '-SinPausa' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argumentos
    exit
}

# Registro de la instalación, para diagnosticar problemas
Start-Transcript -Path (Join-Path $env:TEMP 'CamSolid-instalar.log') -Force | Out-Null

$origen = $PSScriptRoot
$destino = Join-Path $env:ProgramFiles 'CamSolid'
$version = (Get-Content (Join-Path $origen 'version.txt') -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not $version) { $version = '0.0.0' }

Write-Host "Instalando CamSolid $version en $destino"

# 1. Cerrar CamSolid si está abierto
$camsolid = Join-Path $destino 'CamSolid.exe'
foreach ($exe in @($camsolid, (Join-Path $origen 'CamSolid.exe'))) {
    if (Test-Path $exe) { & $exe --stop | Out-Null }
}
Get-Process CamSolid -ErrorAction SilentlyContinue | ForEach-Object { $_.WaitForExit(10000) | Out-Null }

# 2. Detener el servicio de cámaras de Windows para poder reemplazar la DLL.
#    Las aplicaciones que estén usando una cámara la perderán un momento.
Write-Host 'Deteniendo el servicio de cámaras de Windows...'
Stop-Service FrameServerMonitor -Force -ErrorAction SilentlyContinue
Stop-Service FrameServer -Force -ErrorAction SilentlyContinue

# 3. Copiar los archivos
New-Item -ItemType Directory -Force -Path $destino | Out-Null
$archivos = 'CamSolid.exe', 'CamSolidSource.dll', 'CamSolidTest.exe', 'CamSolid.apk',
            'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'adb-NOTICE.txt',
            'desinstalar.ps1', 'version.txt', 'LEEME.txt'
foreach ($archivo in $archivos) {
    $ruta = Join-Path $origen $archivo
    if (Test-Path $ruta) { Copy-Item $ruta $destino -Force }
}

# 4. Registrar la cámara en Windows (Frame Server lee la DLL desde aquí)
$dll = Join-Path $destino 'CamSolidSource.dll'
$registro = Start-Process "$env:WINDIR\System32\regsvr32.exe" -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
if ($registro.ExitCode -ne 0) { throw "No se pudo registrar la cámara (regsvr32 devolvió $($registro.ExitCode))" }

Start-Service FrameServer -ErrorAction SilentlyContinue

# 5. Accesos directos en el menú Inicio y el escritorio
$shell = New-Object -ComObject WScript.Shell
foreach ($carpeta in @("$env:ProgramData\Microsoft\Windows\Start Menu\Programs", "$env:PUBLIC\Desktop")) {
    $acceso = $shell.CreateShortcut((Join-Path $carpeta 'CamSolid.lnk'))
    $acceso.TargetPath = $camsolid
    $acceso.WorkingDirectory = $destino
    $acceso.Description = 'Usa el celular como webcam'
    $acceso.Save()
}

# 6. Entrada en "Agregar o quitar programas"
$clave = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CamSolid'
New-Item -Path $clave -Force | Out-Null
$tamano = [int]((Get-ChildItem $destino -File | Measure-Object -Property Length -Sum).Sum / 1KB)
$valores = @{
    DisplayName     = 'CamSolid'
    DisplayVersion  = $version
    Publisher       = 'CamSolid'
    InstallLocation = $destino
    DisplayIcon     = $camsolid
    UninstallString = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$destino\desinstalar.ps1`""
}
foreach ($nombre in $valores.Keys) { Set-ItemProperty -Path $clave -Name $nombre -Value $valores[$nombre] }
Set-ItemProperty -Path $clave -Name NoModify -Value 1 -Type DWord
Set-ItemProperty -Path $clave -Name NoRepair -Value 1 -Type DWord
Set-ItemProperty -Path $clave -Name EstimatedSize -Value $tamano -Type DWord

Write-Host ''
Write-Host 'CamSolid quedó instalado.'
Write-Host 'Ábrelo desde el menú Inicio o el escritorio y elige la cámara "CamSolid" en tu aplicación.'
Stop-Transcript | Out-Null
if (-not $SinPausa) { Read-Host 'Pulsa Enter para cerrar' }
