@echo off
IF NOT "%~1" == "" IF NOT "%~2" == "" GOTO HaveArgs

echo Usage: sign-installer.bat ^<PFX File^> ^<PFX File Password^>
echo   The Visual Studio component "SignTool.exe" must be in the PATH
GOTO:eof

:HaveArgs
SignTool sign /f "%1" /p "%2" /t http://timestamp.digicert.com jcp_installer.exe
SignTool verify /pa /tw jcp_installer.exe
