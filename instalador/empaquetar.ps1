# Compila CamSolid y arma el paquete de instalación en D:\CamSolid\dist:
#   dist\CamSolid\           carpeta lista para instalar (Instalar.cmd)
#   dist\CamSolid-<v>.zip    la misma carpeta comprimida, para distribuirla
#
# Uso: .\empaquetar.ps1 [-Version 0.1.0]
param([string]$Version = '0.1.0')
$ErrorActionPreference = 'Stop'

$raiz = Split-Path $PSScriptRoot -Parent
$salida = Join-Path $raiz 'dist\CamSolid'
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$platformTools = "$env:LOCALAPPDATA\Android\Sdk\platform-tools"

Write-Host '1. Compilando la app del PC...'
& $cmake --build (Join-Path $raiz 'pc\build') --config Release -- /nologo /v:minimal | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Falló la compilación del PC' }

Write-Host '2. Compilando la app del celular...'
$env:JAVA_HOME = 'D:\AndroidStudio\jbr'
Push-Location (Join-Path $raiz 'android')
& .\gradlew.bat assembleDebug --console=plain -q 2>&1 | Where-Object { $_ -match '^e: |FAILED' }
Pop-Location
if ($LASTEXITCODE -ne 0) { throw 'Falló la compilación de Android' }

Write-Host "3. Armando $salida..."
if (Test-Path $salida) { Remove-Item $salida -Recurse -Force }
New-Item -ItemType Directory -Force -Path $salida | Out-Null

$release = Join-Path $raiz 'pc\build\Release'
Copy-Item "$release\CamSolid.exe", "$release\CamSolidSource.dll", "$release\CamSolidTest.exe" $salida
Copy-Item (Join-Path $raiz 'android\app\build\outputs\apk\debug\app-debug.apk') (Join-Path $salida 'CamSolid.apk')
Copy-Item "$platformTools\adb.exe", "$platformTools\AdbWinApi.dll", "$platformTools\AdbWinUsbApi.dll" $salida
Copy-Item "$platformTools\NOTICE.txt" (Join-Path $salida 'adb-NOTICE.txt')
Copy-Item (Join-Path $PSScriptRoot '*') $salida -Exclude 'empaquetar.ps1'
Set-Content (Join-Path $salida 'version.txt') $Version -Encoding ascii

$zip = Join-Path $raiz "dist\CamSolid-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $salida -DestinationPath $zip

Write-Host ''
Get-ChildItem $salida | Select-Object Name, @{ n = 'KB'; e = { [math]::Round($_.Length / 1KB) } } | Format-Table -AutoSize | Out-String | Write-Host
Write-Host ("Paquete: {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
