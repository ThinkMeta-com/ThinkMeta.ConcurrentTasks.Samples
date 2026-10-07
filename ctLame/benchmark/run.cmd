@echo off
rem ctLame against lame.exe on a folder of WAV files; the report goes to results\.
rem   run.cmd D:\Musik
rem   run.cmd D:\Musik -Rounds 3 -Sequential -EncodeArgs "-V 2"
rem Without a folder the script asks for one.
rem Exit code of the script: 0 all identical, 2 a file differs or failed, 1 other errors.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Compare-Folder.ps1" %*
set rc=%errorlevel%
pause
exit /b %rc%
