; SPDX-License-Identifier: GPL-2.0-only
;
; The installer a user downloads: one file, nothing to unpack.
;
; Built with NSIS, which runs natively on Linux and is an ordinary Ubuntu
; package, so the release still comes off one machine. That was the reason
; for choosing it over the alternatives: WiX needs Wine to produce an MSI,
; and Inno Setup needs Wine full stop. Writing the container by hand was
; considered and rejected, because compression, unpacking, the permission
; prompt, the installed programs list and a working uninstaller are all
; solved problems and none of them is specific to this hardware.
;
; The division of labour is deliberate:
;
;   here                  everything any installer does
;   driversetup.exe       everything only this hardware needs
;
; So the driver logic, which is where the real knowledge is, stays in a
; program that can be run and debugged on its own against a source build.

; Paths above use forward slashes on purpose: this is compiled by a native
; Linux makensis, which resolves them and does not reliably resolve the
; other kind. Paths that appear at runtime, under $INSTDIR, are Windows
; paths and are written the Windows way.

Unicode true
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
; Windows' own version field is four numbers and nothing else, so a tag like
; "v0.1.0" cannot be used for it directly and is passed separately rather
; than parsed here, where there is no good way to fail.
!ifndef NUMERIC
  !define NUMERIC "0.0.0"
!endif
!ifndef PAYLOAD
  !define PAYLOAD "../build/package"
!endif

!define NAME    "USB Display driver"
!define COMPANY "usbdisplay"
!define KEY     "Software\Microsoft\Windows\CurrentVersion\Uninstall\usbdisplay"

Name "${NAME}"
OutFile "../build/usbdisplay-setup.exe"
InstallDir "$PROGRAMFILES64\usbdisplay"
InstallDirRegKey HKLM "${KEY}" "InstallLocation"

; Installing a driver package is not something a user-level process may do,
; and asking for the rights up front means Windows raises the prompt rather
; than the install failing halfway through.
RequestExecutionLevel admin

VIProductVersion "${NUMERIC}.0"
VIAddVersionKey "ProductName" "${NAME}"
VIAddVersionKey "CompanyName" "${COMPANY}"
VIAddVersionKey "FileDescription" "${NAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-2.0-only"

!include "MUI2.nsh"
!include "x64.nsh"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_TEXT "The adapter should appear as a second monitor \
within a few seconds. Give it a little longer if it was only just plugged \
in.$\r$\n$\r$\nA brightness control now runs in the notification area, and \
will start with Windows.$\r$\n$\r$\nIf no monitor appears, \
C:\Windows\Temp\usbdisplaydd.log records every step the driver took and \
where it stopped."

!insertmacro MUI_PAGE_LICENSE "../../LICENSE"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; The driver is built for x64 only, so saying so here is better than
; installing successfully on a machine where nothing can then load.
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This driver is built for 64-bit Windows, and \
this machine is not running it."
    Abort
  ${EndIf}
FunctionEnd

Section "install"
  SetOutPath "$INSTDIR"

  ; Everything the driver step needs, in the layout it expects: each package
  ; is its own directory, because that is what Windows is handed and what
  ; the catalog covering it was made for.
  File /r "${PAYLOAD}/driver"
  File /r "${PAYLOAD}/winusb"
  File "${PAYLOAD}/usbdisplay.cer"
  File "${PAYLOAD}/driversetup.exe"

  File "${PAYLOAD}/tools/usbdisplayctl.exe"
  File "${PAYLOAD}/tools/usbdisplaytray.exe"
  File "../README.md"
  File "../../LICENSE"

  DetailPrint "Installing the driver..."
  ; /nowait because there is nobody at a console to press a key, and the
  ; output is shown in the details pane instead.
  nsExec::ExecToLog '"$INSTDIR\driversetup.exe" /nowait'
  Pop $0
  ${If} $0 <> 0
    DetailPrint "The driver step failed. Nothing has been left half installed."
    Abort "The driver could not be installed. The details above say why."
  ${EndIf}

  ; The tools stay; the packages do not. Once Windows has copied them into
  ; its driver store, a second copy on disk is only something to go stale.
  RMDir /r "$INSTDIR\driver"
  RMDir /r "$INSTDIR\winusb"

  ; Per user, because it is that user's brightness and it needs no rights of
  ; its own to change it.
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" \
    "usbdisplay brightness" '"$INSTDIR\usbdisplaytray.exe"'
  Exec '"$INSTDIR\usbdisplaytray.exe"'

  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr HKLM "${KEY}" "DisplayName"     "${NAME}"
  WriteRegStr HKLM "${KEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr HKLM "${KEY}" "Publisher"       "${COMPANY}"
  WriteRegStr HKLM "${KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${KEY}" "DisplayIcon"     "$INSTDIR\usbdisplaytray.exe"
  WriteRegStr HKLM "${KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKLM "${KEY}" "URLInfoAbout" \
    "https://github.com/nebuloss/winusbdisplay"
  WriteRegDWORD HKLM "${KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${KEY}" "NoRepair" 1
SectionEnd

Section "uninstall"
  ; The driver step first: it stops the brightness control and retires the
  ; device nodes, and removing the files under it would leave Windows with a
  ; driver package pointing at nothing.
  nsExec::ExecToLog '"$INSTDIR\driversetup.exe" /uninstall /nowait'
  Pop $0

  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" \
    "usbdisplay brightness"

  Delete "$INSTDIR\usbdisplayctl.exe"
  Delete "$INSTDIR\usbdisplaytray.exe"
  Delete "$INSTDIR\driversetup.exe"
  Delete "$INSTDIR\usbdisplay.cer"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "${KEY}"
SectionEnd
