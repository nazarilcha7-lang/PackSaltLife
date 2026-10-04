@echo off
cd /d "%~dp0"
call gradlew.bat clean remapJar
if errorlevel 1 ( echo [ERROR] build failed & pause & exit /b 1 )
copy /Y "build\libs\coords-overlay-final-1.0.0.jar" "..\cpp_overlay\mcagent.jar"
echo Done: cpp_overlay\mcagent.jar
pause
