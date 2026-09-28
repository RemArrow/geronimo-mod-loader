@echo off
rem Builds the Geronimo Mod Loader in a BepInEx-style layout.
rem
rem   build\game\                      mirrors <game>\Geronimo\Binaries\Win64\
rem     version.dll                    doorstop (src\doorstop)
rem     doorstop_config.ini
rem     GML\core\GML.dll               the loader (src)
rem     GML\plugins\<Name>\<Name>.dll  from plugins\<Name>\ if you keep your own plugins here
rem     GML\patchers\<Name>.dll        from patchers\<Name>\ likewise
rem   build\gmlcheck.exe               offline checker (tools)
rem   build\examples\<Name>\<Name>.dll example plugins (examples) - never installed
rem   build\tests\<Name>\<Name>.dll    test plugins/patchers (tests) - never installed
rem
rem Usage:  build.bat            build
rem         build.bat install    build and copy into the game (game must be closed)
rem         build.bat package    build and write the release zips to build\release\
rem Set GML_GAME_DIR to your ...\Geronimo\Binaries\Win64 if the game is not in the default Steam library.
setlocal
set ROOT=%~dp0
set OUT=%ROOT%build
set G=%OUT%\game
set GAME=C:\Program Files (x86)\Steam\steamapps\common\GERONIMO\Geronimo\Binaries\Win64
if defined GML_GAME_DIR set GAME=%GML_GAME_DIR%

if not defined VCToolsInstallDir (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
for %%D in ("%OUT%\obj" "%G%\GML\core" "%G%\GML\plugins" "%G%\GML\patchers") do if not exist "%%~D" mkdir "%%~D"

set CFLAGS=/nologo /std:c++20 /O2 /MT /EHsc /W3 /Zi /DUNICODE /D_UNICODE /I"%ROOT%include"
set LFLAGS=/DEBUG /INCREMENTAL:NO

echo === doorstop (version.dll)
cl %CFLAGS% /LD /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" "%ROOT%src\doorstop\*.cpp" /Fe"%G%\version.dll" ^
   /link /DEF:"%ROOT%src\doorstop\version.def" %LFLAGS% /PDB:"%OUT%\version.pdb" || exit /b 1
copy /y "%ROOT%doorstop_config.ini" "%G%\" >nul

echo === core (GML\core\GML.dll)
cl %CFLAGS% /LD /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" "%ROOT%src\*.cpp" /Fe"%G%\GML\core\GML.dll" ^
   /link %LFLAGS% /OPT:REF /PDB:"%G%\GML\core\GML.pdb" || exit /b 1

echo === gmlcheck.exe
cl %CFLAGS% /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" "%ROOT%tools\gmlcheck.cpp" "%ROOT%src\hook.cpp" "%ROOT%src\scan.cpp" ^
   /Fe"%OUT%\gmlcheck.exe" /link %LFLAGS% || exit /b 1

echo === plugins
if exist "%ROOT%plugins" for /d %%M in ("%ROOT%plugins\*") do (
  if not exist "%G%\GML\plugins\%%~nxM" mkdir "%G%\GML\plugins\%%~nxM"
  call :compile "%%M" "%G%\GML\plugins\%%~nxM\%%~nxM.dll" || exit /b 1
  if exist "%%M\Paks" robocopy "%%M\Paks" "%G%\GML\plugins\%%~nxM\Paks" /MIR /NJH /NJS /NFL /NDL /NP >nul
  if exist "%%M\Assets" robocopy "%%M\Assets" "%G%\GML\plugins\%%~nxM\Assets" /MIR /NJH /NJS /NFL /NDL /NP >nul
)

echo === patchers
if exist "%ROOT%patchers" for /d %%M in ("%ROOT%patchers\*") do (
  call :compile "%%M" "%G%\GML\patchers\%%~nxM.dll" || exit /b 1
)

for %%K in (examples tests) do (
  echo === %%K ^(built, never installed^)
  for /d %%M in ("%ROOT%%%K\*") do (
    if not exist "%OUT%\%%K\%%~nxM" mkdir "%OUT%\%%K\%%~nxM"
    call :compile "%%M" "%OUT%\%%K\%%~nxM\%%~nxM.dll" || exit /b 1
  )
)

rem linker by-products
for /r "%OUT%" %%F in (*.exp *.lib *.ilk) do del /q "%%F"

if /i "%1"=="install" (
  echo === install to "%GAME%"
  copy /y "%G%\version.dll" "%GAME%\" >nul || exit /b 1
  if not exist "%GAME%\doorstop_config.ini" copy /y "%G%\doorstop_config.ini" "%GAME%\" >nul
  robocopy "%G%\GML\core" "%GAME%\GML\core" /MIR /NJH /NJS /NFL /NDL /NP >nul
  if errorlevel 8 exit /b 1
  rem plugins/patchers: add/update, never delete what the user put there; config\ is never touched
  robocopy "%G%\GML\plugins" "%GAME%\GML\plugins" /E /NJH /NJS /NFL /NDL /NP >nul
  if errorlevel 8 exit /b 1
  rem ...except a plugin's Paks\, which mirrors exactly so a removed container doesn't linger
  for /d %%M in ("%G%\GML\plugins\*") do if exist "%%M\Paks" (
    robocopy "%%M\Paks" "%GAME%\GML\plugins\%%~nxM\Paks" /MIR /NJH /NJS /NFL /NDL /NP >nul
  )
  robocopy "%G%\GML\patchers" "%GAME%\GML\patchers" /E /NJH /NJS /NFL /NDL /NP >nul
  if errorlevel 8 exit /b 1
)
if /i "%1"=="package" (
  echo === package
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%package.ps1" || exit /b 1
)
echo === done
exit /b 0

rem A plugin/patcher DLL from the .cpp files in folder %1, written to %2. If the folder has a
rem resource script named after the DLL (<Name>.rc), it is compiled in: files it lists as RCDATA
rem ship inside the DLL (GML_API::PluginResource). Paths in it are relative to the folder.
:compile
set RES=
if exist "%~1\%~n2.rc" (
  rc /nologo /I "%~1" /fo"%OUT%\obj\%~n2.res" "%~1\%~n2.rc" || exit /b 1
  set RES="%OUT%\obj\%~n2.res"
)
cl %CFLAGS% /LD /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" "%~1\*.cpp" %RES% /Fe"%~2" /link %LFLAGS% || exit /b 1
exit /b 0
