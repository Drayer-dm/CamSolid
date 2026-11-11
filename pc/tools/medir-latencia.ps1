# Mide la latencia con el celular conectado por USB, sin pasar por Frame Server.
# Carga la DLL recién compilada con CamSolidSelfTest y lee las estadísticas que
# la app del celular escribe en su registro (logcat).
#
# Uso: .\medir-latencia.ps1 [-Formato 0] [-Opciones "--ez estabilizacion true"] [-Segundos 12] [-Carpeta <ruta>]
#   Formato: 0 = 1920x1080 a 30 (predeterminado), 1 = 1280x720 a 30
#   Opciones: extras para abrir la app (ver readTestOptions en MainActivity.kt)
param(
    [int]$Formato = 0,
    [string]$Opciones = '',
    [int]$Segundos = 12,
    [string]$Carpeta = $env:TEMP
)

$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$release = Join-Path $PSScriptRoot '..\build\Release'
$nombres = @('1920x1080 a 30', '1280x720 a 30')

& $adb forward tcp:27183 tcp:27183 | Out-Null
& $adb shell am force-stop com.camsolid.app
& $adb shell "am start -n com.camsolid.app/.MainActivity $Opciones" | Out-Null
Start-Sleep -Seconds 2
& $adb logcat -c

$imagen = Join-Path $Carpeta "camsolid_prueba.bmp"
$salida = & "$release\CamSolidSelfTest.exe" "$release\CamSolidSource.dll" $imagen $Segundos $Formato

# Se descartan los primeros 3 segundos: la cámara y los relojes se están ajustando
$lineas = & $adb logcat -d -s CamSolid:I | Select-String 'STATS' | Select-Object -Skip 3
$valores = foreach ($linea in $lineas) {
    $campos = @{}
    foreach ($m in [regex]::Matches($linea.Line, '(\w+)=(-?[\d.,]+)')) {
        $campos[$m.Groups[1].Value] = [double]($m.Groups[2].Value -replace ',', '.')
    }
    [pscustomobject]$campos
}

$titulo = if ($Opciones) { "$($nombres[$Formato]) [$Opciones]" } else { $nombres[$Formato] }
if (-not $valores) {
    "{0}: sin estadísticas. Salida de la prueba: {1}" -f $titulo, ($salida -join ' ')
    return
}

$promedio = { param($campo) ($valores | Measure-Object -Property $campo -Average).Average }
$latencia = & $promedio 'latencia'
$camara = & $promedio 'camara'
$salidaCodificador = & $promedio 'salidaCodificador'
"{0}: latencia {1:N1} ms | {2:N1} cuadros/s | camara {3:N1} ms | codificador {4:N1} ms | PC {5:N1} ms" -f
    $titulo, $latencia, (& $promedio 'fps'), $camara, ($salidaCodificador - $camara), ($latencia - $salidaCodificador)
"    " + ($salida | Select-String 'cuadros en')
