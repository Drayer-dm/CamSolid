@echo off
rem Abre el instalador de CamSolid. Windows pedira permisos de administrador.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalar.ps1"
