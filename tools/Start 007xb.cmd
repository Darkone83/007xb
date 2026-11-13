@echo off
cd /d "%~dp0"
if exist "%~dp0dist\007xb Asset Studio.exe" (
 start "" "%~dp0dist\007xb Asset Studio.exe"
 exit /b 0
)
where pyw >nul 2>nul
if %errorlevel% equ 0 (
 start "" pyw -3 "%~dp0007xb Asset Studio.pyw"
 exit /b 0
)
where pythonw >nul 2>nul
if %errorlevel% equ 0 (
 start "" pythonw "%~dp0007xb Asset Studio.pyw"
 exit /b 0
)
echo Python 3 with Tkinter is required to run Asset Studio.
pause
