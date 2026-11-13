@echo off
setlocal
rem All packaging tools, scripts, and build products are inside tools\.
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
 echo Python 3 is required to build the Windows executable.
 pause
 exit /b 1
)
py -3 -m PyInstaller --version >nul 2>nul
if errorlevel 1 (
 echo PyInstaller is required. Install it with: py -3 -m pip install pyinstaller
 pause
 exit /b 1
)
rem Only converter code and metadata are bundled; NEVER bundle ROMs, .dat or cache.
py -3 -m PyInstaller --noconfirm --clean --onefile --windowed ^
 --name "007xb Asset Studio" ^
 --paths "%~dp0." ^
 --add-data "%~dp0007xb_assets.py;tools" ^
 --add-data "%~dp0007xb_converter_reference.zip;tools" ^
 --add-data "%~dp0branding\darkone83-wireframe.obj;tools/branding" ^
 --distpath "%~dp0dist" ^
 --workpath "%~dp0.cache\pyinstaller\build" ^
 --specpath "%~dp0.cache\pyinstaller" ^
 "%~dp0007xb Asset Studio.pyw"
if errorlevel 1 (
 echo EXE build failed.
 pause
 exit /b 1
)
echo.
echo Built: tools\dist\007xb Asset Studio.exe
echo Copy the EXE into a tools folder within your 007xb directory.
pause
