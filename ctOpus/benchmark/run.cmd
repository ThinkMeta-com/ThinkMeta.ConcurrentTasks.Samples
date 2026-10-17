@echo off
rem ctOpus against opusenc on a folder of WAV files; the report goes to results\.
rem   run.cmd D:\Music
rem   run.cmd D:\Music -EncodeArgs "--comp 10" -ReferenceArgs "--comp 0"
rem   run.cmd D:\Music -Sequential -Rounds 3
rem Without a folder argument the script asks for one. Exit code 0: every file encoded and
rem bit-identical; 2: a file differs or was not encoded; 1: other errors (missing programs,
rem bad parameters, abort).
setlocal
rem Started from PowerShell 7, Windows PowerShell 5.1 would inherit its module path and load the
rem wrong Microsoft.PowerShell.Utility (Get-FileHash and others then fail); 5.1 rebuilds it if empty.
set PSModulePath=
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Compare-Folder.ps1" %*
set rc=%errorlevel%
pause
exit /b %rc%
