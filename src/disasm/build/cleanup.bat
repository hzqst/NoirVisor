@echo off

title NoirVisor Cleanup
echo Project: NoirVisor, Zydis
echo Platform: Universal (Non-Binary Build)
echo Preset: Cleanup
echo Powered by zero.tangptr@gmail.com
echo Warning: All compiled binaries, including intermediate files, will be deleted!
pause.

echo Performing cleanup...
del ..\bin /q /s

if "%~1"=="/s" (echo Cleanup Completed!) else (pause)