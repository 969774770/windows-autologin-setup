@echo off

echo [1/3] Prepare local CRT dir (override default manifest)...
if not exist local-crt mkdir local-crt

echo [2/3] Compile resources (requireAdministrator manifest)...
windres app.rc -O coff -o appres.o
if errorlevel 1 goto :err
windres app.rc -O coff -o local-crt\default-manifest.o
if errorlevel 1 goto :err

echo [3/3] Compile AutoLogin.exe ...
gcc -mwindows -O2 -Wall -Wextra "-Blocal-crt/" -o AutoLogin.exe autologin.c appres.o -lshell32 -ladvapi32
if errorlevel 1 goto :err

del appres.o 2>nul
echo.
echo Build OK: AutoLogin.exe
exit /b 0

:err
echo.
echo Build FAILED. Check errors above.
exit /b 1
