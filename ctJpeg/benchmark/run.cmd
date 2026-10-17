@echo off
rem ctJpeg against jpegli, mozjpeg and libjpeg-turbo on a folder of PPM/PGM images; the report goes to results\.
rem   run.cmd D:\Images
rem   run.cmd D:\Images -Quality 90 -Simd sse2
rem   run.cmd D:\Images -Sequential -Rounds 3
rem Without a folder argument the script asks for one.
rem Exit code: 0 all images bit-identical, 2 an image differs or failed, 1 other errors.
rem Started from PowerShell 7, Windows PowerShell 5.1 would inherit its module path and load the
rem wrong Microsoft.PowerShell.Utility (Get-FileHash not found): clear it for this script.
setlocal
set PSModulePath=
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Compare-Folder.ps1" %*
set rc=%errorlevel%
pause
exit /b %rc%
