@echo off
setlocal
python "%~dp0audio_rcf_extract.py" %*
exit /b %errorlevel%
