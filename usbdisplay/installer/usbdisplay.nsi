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
!include "LogicLib.nsh"
; Where WM_CLOSE comes from. Included by name rather than relied on through
; another header, because which headers pull in which is not stable.
!include "WinMessages.nsh"

!define MUI_ABORTWARNING

; The licence is GPL-2.0 and this is a derived work, so the terms are shown
; and have to be accepted rather than merely shipped in the directory. A copy
; is installed alongside the programs as well, because an accept button is
; not somewhere anyone can go back and read.
!define MUI_LICENSEPAGE_BUTTON "Agree"
!define MUI_LICENSEPAGE_TEXT_BOTTOM "This software is free software under the \
GNU General Public License, version 2. You may use, study, share and modify \
it. Select Agree to continue."

!define MUI_FINISHPAGE_TEXT "The adapter should appear as a second monitor \
within a few seconds. Give it a little longer if it was only just plugged \
in.$\r$\n$\r$\nA brightness control now runs in the notification area, and \
will start with Windows.$\r$\n$\r$\nIf no monitor appears, \
C:\Windows\Temp\usbdisplaydd.log records every step the driver took and \
where it stopped."

!insertmacro MUI_PAGE_LICENSE "../../LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Asks the brightness control to close, and waits for it to.
;
; This has to happen before anything is written, and it is the whole reason
; the ordering here matters: on an upgrade that program is running, and
; Windows will not overwrite a program that is in memory. Left until later it
; fails at the first file, having already asked the user for permission.
;
; WM_CLOSE rather than terminating it, so it takes its icon out of the
; notification area on the way; killed instead, the icon stays there until
; something makes the shell notice, which can be hours.
!macro StopTheTray
  Push $0
  Push $1
  StrCpy $1 0
  ${Do}
    FindWindow $0 "UsbDisplayBrightnessTray"
    ${If} $0 == 0
      ${ExitDo}
    ${EndIf}
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 500
    IntOp $1 $1 + 1
    ; Ten tries is five seconds. If it is still there after that it is wedged,
    ; and the file copy will report that plainly enough.
    ${If} $1 >= 10
      ${ExitDo}
    ${EndIf}
  ${Loop}
  Pop $1
  Pop $0
!macroend

Function .onInit
  ; The driver is built for x64 only, so saying so here is better than
  ; installing successfully on a machine where nothing can then load.
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This driver is built for 64-bit Windows, and \
this machine is not running it."
    Abort
  ${EndIf}

  ; Every registry access from here on is to the real HKLM\Software.
  ;
  ; Without this they are not. NSIS produces a 32-bit program, and Windows
  ; quietly redirects a 32-bit process writing to HKLM\Software into
  ; HKLM\Software\WOW6432Node. Nothing fails; the values simply go somewhere
  ; else. This was found on a real machine: the entry in the installed
  ; programs list still showed the previous version, because the new one had
  ; been written to the other hive, and the old entry was left behind
  ; pointing at a directory that no longer existed and so could not be
  ; removed.
  ;
  ; Everything else this installs is 64-bit and writes to the real hive, so
  ; the installer has to agree with it.
  SetRegView 64

  ; Where a previous version put itself, if it did.
  ReadRegStr $0 HKLM "${KEY}" "InstallLocation"
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\*.*"
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd

Function un.onInit
  ; The uninstaller is a separate program and gets none of the above.
  SetRegView 64
FunctionEnd

Section "install"
  DetailPrint "Making room..."
  !insertmacro StopTheTray

  ; An entry written by a version that landed in the wrong hive, and one
  ; written by a version that installed itself somewhere else entirely.
  ; Neither can be removed by the user once its files are gone, so an
  ; upgrade has to clear up after its own past rather than accumulate
  ; entries that do nothing but fail.
  SetRegView 32
  DeleteRegKey HKLM "${KEY}"
  SetRegView 64

  SetOutPath "$INSTDIR"

  ; Left by a version that installed more than this one does. Deleting it
  ; here rather than leaving it means an upgrade does not accumulate.
  Delete "$INSTDIR\brightnessprobe.exe"

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
  ;
  ; Written to the hive of whoever is running this, which is right when an
  ; administrator is prompted for consent and wrong when a standard user
  ; types someone else's credentials: the entry then belongs to the account
  ; that authorised the install rather than the one using the machine. The
  ; second case is rare enough, and the consequence small enough, that the
  ; alternative of writing every loaded user hive is not worth it. Anyone
  ; affected can start the program once from the Start menu.
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" \
    "usbdisplay brightness" '"$INSTDIR\usbdisplaytray.exe"'

  ; Started through Explorer rather than directly, because this program is
  ; elevated and anything it launches inherits that. A notification area icon
  ; running as administrator is the wrong thing on its own terms, and it also
  ; would not be the same program the Run entry above starts at sign-in, so
  ; the two would behave differently. Explorer runs as the signed-in user,
  ; so what it launches does too.
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\usbdisplaytray.exe"'

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
  ; The brightness control first, for the same reason as on the way in: a
  ; running program cannot be deleted, and here the failure would be an
  ; uninstall that silently leaves things behind.
  !insertmacro StopTheTray

  ; Then the driver step, before its own file is removed: it retires the
  ; device nodes and the driver packages, and deleting what it needs first
  ; would leave Windows holding a package that points at nothing.
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
