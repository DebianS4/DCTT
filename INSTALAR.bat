@echo off
rem ==========================================================
rem  DCTT - INSTALADOR UNICO para Windows XP SP3 (32 bits)
rem  Copia, compila DCTT.exe con su icono y crea el acceso directo.
rem  Necesita gcc.exe de MinGW (y opcionalmente windres) en el PATH.
rem ==========================================================
setlocal
set DEST=%USERPROFILE%\DCTT
set VBS=%TEMP%\dctt-acceso.vbs
set RC=%TEMP%\dctt.rc

echo.
echo   DCTT - Diagnostico y Codificacion Taller TT
echo   ------------------------------------------
echo   Destino: %DEST%
echo.

if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%~dp0dctt.cpp" "%DEST%" >nul
if exist "%DEST%\averias.csv" goto keepcsv
copy /Y "%~dp0averias.csv" "%DEST%" >nul
:keepcsv
rem Si traes la ftd2xx.dll en la carpeta, se copia sola junto al .exe.
if exist "%~dp0ftd2xx.dll" copy /Y "%~dp0ftd2xx.dll" "%DEST%" >nul
copy /Y "%~dp0LEEME.txt" "%DEST%" >nul
copy /Y "%~dp0dctt.ico" "%DEST%" >nul

gcc --version >nul 2>&1
if errorlevel 1 goto nogcc

rem --- aviso si el compilador no genera binarios de 32 bits ---
gcc -dumpmachine 2>nul | find "64" >nul
if not errorlevel 1 echo   Aviso: este gcc es de 64 bits; XP necesita un MinGW de 32 bits (i686).

cd /d "%DEST%"
set RES=
windres --version >nul 2>&1
if errorlevel 1 goto sinicono

> "%RC%" echo 1 ICON "dctt.ico"
windres "%RC%" -O coff -o dctt.res
if errorlevel 1 goto sinicono
set RES=dctt.res
echo   Icono incrustado.
goto compilar

:sinicono
echo   Aviso: windres no disponible, el .exe saldra sin icono.

:compilar
echo   Compilando DCTT.exe para Windows XP ...
rem -static y -s: sin DLL de MinGW y sin tabla de simbolos.
rem -mwindows: aplicacion de ventana, sin consola.
gcc -O2 -s -static -mwindows -DWINVER=0x0501 -D_WIN32_WINNT=0x0501 -D_WIN32_IE=0x0500 -o DCTT.exe dctt.cpp %RES% -lwinmm -lgdi32 -luser32
if errorlevel 1 goto fallo

rem Acceso directo: se genera el script al vuelo, sin archivos extra
> "%VBS%" echo Set s = CreateObject("WScript.Shell")
>> "%VBS%" echo Set l = s.CreateShortcut(s.SpecialFolders("Desktop") + "\DCTT.lnk")
>> "%VBS%" echo l.TargetPath = s.ExpandEnvironmentStrings("%USERPROFILE%") + "\DCTT\DCTT.exe"
>> "%VBS%" echo l.WorkingDirectory = s.ExpandEnvironmentStrings("%USERPROFILE%") + "\DCTT"
>> "%VBS%" echo l.IconLocation = s.ExpandEnvironmentStrings("%USERPROFILE%") + "\DCTT\DCTT.exe, 0"
>> "%VBS%" echo l.Description = "DCTT - Audi TT 8N"
>> "%VBS%" echo l.Save
cscript //nologo "%VBS%" >nul
del "%VBS%" >nul 2>&1

echo.
echo   Listo. DCTT.exe esta en %DEST%
echo   Acceso directo en el escritorio, con el icono de DCTT.
echo.
echo   Comprobacion de XP: copia ftd2xx.dll junto al .exe y ejecuta
echo   DCTT sin cable. Si la ventana abre, el binario es compatible.
goto fin

:nogcc
echo.
echo   No se encontro gcc.exe en el PATH.
echo   Instala MinGW de 32 bits y vuelve a ejecutar este archivo.
goto fin

:fallo
echo.
echo   Fallo la compilacion: revisa los mensajes de gcc.

:fin
echo.
pause
