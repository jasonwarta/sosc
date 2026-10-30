@echo off
rem Copies Streets of SimCity into this folder, laid out the way the fix expects: the CD's exe
rem folder at the top, plus the game's data folders. Either
rem   - drag the CD image (.iso) or the oldgamesdownload.com zip onto this file, or
rem   - mount the ISO (double-click it) or insert the CD, then double-click this file.
setlocal
set "DEST=%~dp0"
set "DEST=%DEST:~0,-1%"
if not "%~1"=="" if exist "%~1\" (set "SRC=%~1") else (goto image)
if not defined SRC for %%d in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%d:\exe\Streets.exe" if exist "%%d:\GEO" set "SRC=%%d:"
)
if not defined SRC (
    echo Couldn't find the Streets of SimCity CD. Drag the .iso ^(or the zip it came in^) onto
    echo this file, or double-click the ISO to mount it and run this again.
    pause
    exit /b 1
)
echo Copying Streets of SimCity from %SRC% into %DEST% ...
robocopy "%SRC%\exe" "%DEST%" /E /NFL /NDL /NJH /NJS /NP
if errorlevel 8 goto failed
for %%f in (BMP Cities FONTS Games GEO HELP scenarios shared SMK SOUND Weapons X) do (
    robocopy "%SRC%\%%f" "%DEST%\%%f" /E /NFL /NDL /NJH /NJS /NP
    if errorlevel 8 goto failed
)
rem files from a CD are read-only; the fix needs to update a few (the moose)
attrib -r "%DEST%\*" /s /d >nul
rem Streets.exe won't start without a glide2x.dll beside it (the CD keeps it in shared)
if not exist "%DEST%\glide2x.dll" copy "%DEST%\shared\glide2x.dll" "%DEST%\" >nul
goto done

:image
echo Copying Streets of SimCity from %~nx1 into %DEST% (about a minute) ...
powershell -NoProfile -ExecutionPolicy Bypass -File "%DEST%\install_game.ps1" -Source "%~f1" -Dest "%DEST%"
if errorlevel 1 (
    echo Copying failed; see logs\install_game.log.
    pause
    exit /b 1
)

:done
echo Done. Start the game with "Play (OpenGL).cmd".
pause
exit /b 0
:failed
echo Copying failed (see above).
pause
exit /b 1
