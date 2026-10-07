@echo off
rem ctJpeg against jpegli, mozjpeg and libjpeg-turbo on a folder of PPM/PGM images; the report goes to results\.
rem   run.cmd D:\Images
rem   run.cmd D:\Images -Quality 90 -Simd sse2
rem   run.cmd D:\Images -Sequential -Rounds 3
rem Without a folder argument the script asks for one.
rem Exit code: 0 all images bit-identical, 2 an image differs or failed, 1 other errors.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Compare-Folder.ps1" %*
set rc=%errorlevel%
pause
exit /b %rc%
