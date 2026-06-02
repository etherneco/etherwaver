@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64
dumpbin /DEPENDENTS "C:\projects.local\etherwaver\build\bin\Release\waver.exe" | findstr /i "\.dll"
