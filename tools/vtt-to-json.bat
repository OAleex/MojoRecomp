@echo off
setlocal
python "%~dp0vtt_to_json.py" %*
exit /b %errorlevel%
