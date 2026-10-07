@echo off
rem ctDeflate against libdeflate 1.26 gzip.exe; the report goes to results\.
rem   run.cmd                                   asks for a file or folder (or uses Bench-Files.txt)
rem   run.cmd D:\Data\big.bin D:\Logs -Levels 1,6,9 -Rounds 3
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Compare-Gzip.ps1" %*
set rc=%errorlevel%
pause
exit /b %rc%
