@echo off
rem Streets of SimCity with the OpenGL renderer. dsound.dll in this folder loads streets_fix.dll.
start "" /d "%~dp0" "%~dp0Streets.exe" -o2
