# Guarda una imagen PNG de la ventana de CamSolid, aunque esté detrás de otras.
# Uso: .\captura-ventana.ps1 <ruta.png>
param([Parameter(Mandatory)][string]$Ruta)

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Ventana {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string clase, string titulo);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr ventana, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr ventana, IntPtr dc, uint opciones);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

$ventana = [Ventana]::FindWindow('CamSolidWindow', [NullString]::Value)
if ($ventana -eq [IntPtr]::Zero) { 'No se encontró la ventana de CamSolid'; exit 1 }
$rect = New-Object Ventana+RECT
[Ventana]::GetWindowRect($ventana, [ref]$rect) | Out-Null
$imagen = New-Object System.Drawing.Bitmap(($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
$grafico = [System.Drawing.Graphics]::FromImage($imagen)
$dc = $grafico.GetHdc()
# 2 = PW_RENDERFULLCONTENT: incluye el contenido dibujado por la GPU
[Ventana]::PrintWindow($ventana, $dc, 2) | Out-Null
$grafico.ReleaseHdc($dc)
$imagen.Save($Ruta, [System.Drawing.Imaging.ImageFormat]::Png)
$grafico.Dispose()
$imagen.Dispose()
"Captura guardada en $Ruta"
