; JCP Installer script
; * Installs jcp.exe to C:\Program Files (x86)\jcp by default
; * TODO: Adds the install directory to the path.
; * Installs libusb0 drivers for the skunkboard

; No need to support Windows 9x, so build a Unicode installer to silence
; deprecation warnings
  Unicode               true

; Use modern interface
  !include MUI2.nsh
  !define MUI_FINISHPAGE_NOAUTOCLOSE

; General
  Name                  "Jaguar Copy"
  OutFile               "jcp_installer.exe"
  InstallDir            $PROGRAMFILES\jcp
  InstallDirRegKey      HKLM "Software\jcp" "Install_Dir"
  ShowInstDetails       show
  RequestExecutionLevel admin

; Pages
  !insertmacro MUI_PAGE_LICENSE licenses.txt
  !insertmacro MUI_PAGE_COMPONENTS
  !insertmacro MUI_PAGE_DIRECTORY
  !insertmacro MUI_PAGE_INSTFILES
  !define MUI_FINISHPAGE_LINK "Documentation && the latest version on Github"
  !define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/cubanismo/skunk_jcp"
  !define MUI_FINISHPAGE_TEXT "To get started, plug your Skunkboard USB cable \
into the computer and your Skunkboard, then open a command prompt (Click \
the start menu and type 'cmd' then hit Enter), and run 'jcp'.$\n$\n\
NOTE: The driver installation will not be finalized until the Skunkboard is \
connected for the first time, and JCP will not run at all until the driver is \
completely installed.$\n$\n\
Have fun!"
  !insertmacro MUI_PAGE_FINISH
  !insertmacro MUI_UNPAGE_INSTFILES 
  !insertmacro MUI_UNPAGE_FINISH

;Languages
  !insertmacro MUI_LANGUAGE "English"

; Installer
Section "!Jaguar Copy" SecJCP
  SectionInstType RO
  SetOutPath $INSTDIR
  File "..\Release\jcp.exe"
  File "usb-drv-installer.exe"
  WriteRegStr HKLM SOFTWARE\jcp "Install_Dir" "$INSTDIR"
  ; Write the uninstall keys for Windows
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\jcp" "DisplayName" "Jaguar Copy"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\jcp" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\jcp" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\jcp" "NoRepair" 1
  WriteUninstaller "uninstall.exe"
SectionEnd

; Call wdi-simple
;
; -n, --name <name>          set the device name
; -f, --inf <name>           set the inf name
; -m, --manufacturer <name>  set the manufacturer name
; -v, --vid <id>             set the vendor ID (VID)
; -p, --pid <id>             set the product ID (PID)
; -i, --iid <id>             set the interface ID (MI)
; -t, --type <driver_type>   set the driver to install
;                            (0=WinUSB, 1=libusb0, 2=libusbK, 3=usbser, 4=custom)
; -d, --dest <dir>           set the extraction directory
; -x, --extract              extract files only (don't install)
; -c, --cert <certname>      install certificate <certname> from the
;                            embedded user files as a trusted publisher
;     --stealth-cert         installs certificate above without prompting
; -s, --silent               silent mode
; -b, --progressbar=[HWND]   display a progress bar during install
;                            an optional HWND can be specified
; -o, --timeout              set timeout (in ms) to wait for any 
;                            pending installations
; -l, --log                  set log level (0=debug, 4=none)
; -h, --help                 display usage
Section "USB Driver Installation" SecDriver
  DetailPrint "Running $INSTDIR\usb-drv-installer.exe"
  nsExec::ExecToLog '"$INSTDIR\usb-drv-installer.exe" --name "Skunkboard" --vid 0x04b4 --pid 0x7200 --type 1 --progressbar=$HWNDPARENT --timeout 120000'
SectionEnd

; Add the installation directory to the PATH environment variable
Section "Add JCP to Path" SecPath
  ; Write to the system-wide PATH variable, not the current user's
  EnVar::SetHKLM

  ; Add the installation directory to the path
  EnVar::AddValueEx "PATH" "$INSTDIR"
  DetailPrint "JCP installation directory added to 'PATH' environment variable"
SectionEnd

; Uninstaller
Section "Uninstall"
  ; Delete the installer registry entries
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\jcp"
  DeleteRegKey HKLM SOFTWARE\jcp

  ; Delete the installation directory from the path
  EnVar::SetHKLM
  EnVar::DeleteValue "PATH" "$INSTDIR"

  ; Delete the files
  Delete $INSTDIR\jcp.exe
  Delete $INSTDIR\usb-drv-installer.exe
  Delete $INSTDIR\uninstall.exe
  RMDir "$INSTDIR"
SectionEnd

LangString DESC_SecJCP ${LANG_ENGLISH} "Install the Jaguar Copy program (jcp.exe)."
LangString DESC_SecDriver ${LANG_ENGLISH} "Install the libusb0 USB driver for the Skunkboard's USB chipset. The driver is required for JCP to function, so leave this checked unless you plan on installing it manually later, or already have it installed."
LangString DESC_SecPath ${LANG_ENGLISH} "Add the install directory to the system 'PATH' environment variable. This allows running jcp from any directory without typing out its full install path every time."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecJCP} $(DESC_SecJCP)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDriver} $(DESC_SecDriver)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} $(DESC_SecPath)
!insertmacro MUI_FUNCTION_DESCRIPTION_END
