@echo off
rem ctJxl against cjxl (libjxl 0.13.0) on a folder of images; the report goes to results\.
rem   run.cmd conformance
rem   run.cmd D:\Images
rem   run.cmd D:\Images -Cases "-q 90;-d 0 -e 9" -Rounds 3
rem   run.cmd D:\Images -Rounds 3 -Files 2
rem Without a folder argument the script asks for one.
rem Exit code: 0 all encodings bit-identical, 2 an encoding differs or failed, 1 other errors.
rem Started from PowerShell 7, Windows PowerShell 5.1 would inherit its module path and load the
rem wrong Microsoft.PowerShell.Utility (Get-FileHash not found): clear it for this script.
setlocal
set PSModulePath=
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Compare-Folder.ps1" %*
set rc=%errorlevel%
pause
exit /b %rc%
