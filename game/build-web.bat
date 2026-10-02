@echo off
rem Builds the game for the browser. If anything is wrong it says BUILD FAILED loudly and removes the old build,
rem so a stale copy of the game can never be mistaken for the new one.
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "MISSING="
for %%F in (main.cpp shell.html social.h jsonutil.h npc_brain.h npc_brain_weights.h dynamics.h persona.h predictions.h dialogue.json config.json) do (
    if not exist "%%F" set "MISSING=!MISSING! %%F"
)
if defined MISSING (
    echo.
    echo ===== BUILD STOPPED: these files are missing from the game folder:
    echo      !MISSING!
    echo ===== Copy them in and run build-web.bat again.
    exit /b 1
)

if not exist ..\MyProject\wwwroot mkdir ..\MyProject\wwwroot
rem remove the previous build first: if this build fails, nothing stale is left behind to play by mistake
del /q ..\MyProject\wwwroot\index.html ..\MyProject\wwwroot\index.js ..\MyProject\wwwroot\index.wasm ..\MyProject\wwwroot\index.data 2>nul

call em++ main.cpp -std=c++17 -sALLOW_MEMORY_GROWTH=1 -o ..\MyProject\wwwroot\index.html -Os -Iraylib\include -Lraylib\lib -lraylib -sUSE_GLFW=3 -sEXPORTED_RUNTIME_METHODS=stringToUTF8,UTF8ToString --preload-file assets@assets --preload-file config.json@config.json --preload-file dialogue.json@dialogue.json --shell-file shell.html -DPLATFORM_WEB
if errorlevel 1 (
    echo.
    echo ===== BUILD FAILED. The game was NOT updated: read the error above. =====
    exit /b 1
)
echo.
echo ===== BUILD OK. Now press Ctrl+Shift+R on the game page. The title screen shows the build time in its bottom-right corner: it should be NOW. =====