; ============================================================================
;  ShellacFilters VirtualDJ plug-ins - installer
;
;  Two DLLs into VirtualDJ's plug-in folder. That is the whole job, and the
;  script is longer than that only because of three things worth getting right.
;
;  1. It does not elevate, and must not.
;
;     VirtualDJ keeps its plug-ins under a per-user root - %LOCALAPPDATA%\
;     VirtualDJ on 2023 and later, Documents\VirtualDJ before that. An elevated
;     installer resolves those to the *administrator's* profile, so the DLLs
;     land in a folder the VirtualDJ the user is actually running never scans,
;     and the effects simply do not appear. Hence RequestExecutionLevel user,
;     and hence no MSI: a per-machine MSI cannot write a per-user folder
;     correctly and a per-user MSI is a fight with ALLUSERS for no gain.
;
;  2. It cannot overwrite a loaded DLL.
;
;     Once an effect has been switched on, VirtualDJ holds the module until it
;     exits - and on VirtualDJ 2025 that is the *only* time the module is
;     loaded, since the folder scan does not call DllGetClassObject. So a
;     reinstall over a running VirtualDJ fails on the file copy, which is
;     caught rather than ignored: SetOverwrite try sets the error flag instead
;     of aborting, and the user gets a Retry.
;
;  3. It has to be crisp on a high-DPI display.
;
;     ManifestDPIAwareness makes Windows hand us real pixels instead of
;     bitmap-stretching a 96 dpi window, and NSIS then lays the dialogs out in
;     scaled dialog units. Text scales; bitmaps would not, so this installer
;     deliberately has none - no MUI header image, no welcome bitmap. That is
;     also why it looks plain, and it is a trade made on purpose.
;
;  Driven by scripts\package.ps1, which passes DIST, VERSION and OUTFILE. It
;  can be compiled by hand too:
;
;      makensis /DDIST=..\..\dist\vdj\x64 installer\vdj_plugins.nsi
; ============================================================================

Unicode true

!ifndef DIST
  !define DIST "..\..\dist\vdj\x64"
!endif
!ifndef VERSION
  !define VERSION "1.0.1"
!endif
!ifndef VERSION4
  !define VERSION4 "1.0.1.0"
!endif
!ifndef LICENSEFILE
  ; Relative paths in an .nsi resolve against the directory holding the script,
  ; so this works when compiled in place - but package.ps1 passes an absolute
  ; path so that the script still compiles from anywhere.
  !define LICENSEFILE "..\..\..\LICENSE"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\..\dist\vdj\ShellacFilters-VirtualDJ-${VERSION}-x64-Setup.exe"
!endif

!define PRODUCT   "ShellacFilters VirtualDJ plug-ins"
!define PUBLISHER "ShellacFilters (MIT)"
!define REGKEY    "Software\ShellacFilters\VirtualDJ Plugins"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ShellacFiltersVirtualDJ"

; The category folder every VirtualDJ DSP plug-in lives in, buffer ones
; included - VirtualDJ decides what a plug-in is by the IID it answers, not by
; where the file sits. See common/vdj_entry.h.
!define PLUGDIR   "Plugins64\SoundEffect"

Name "${PRODUCT}"
OutFile "${OUTFILE}"
BrandingText "${PRODUCT} ${VERSION}"

; --- no elevation. See note 1 at the top; this is the load-bearing line. -----
RequestExecutionLevel user

; --- high dpi ---------------------------------------------------------------
; PerMonitorV2 so dragging the window between a laptop panel and an external
; display re-lays it out rather than blurring it; system awareness as the
; fallback for Windows 8 and 8.1, which have no per-monitor mode.
ManifestDPIAware true
ManifestDPIAwareness "PerMonitorV2,system"

; --- compression ------------------------------------------------------------
; The smallest NSIS can produce, and /SOLID is the half that earns it. The two
; DLLs are separate builds of largely the same code - the same BufferPipeline,
; the same static CRT - so compressing them as one stream lets the matcher find
; the duplication across the file boundary, which per-file compression cannot
; see. Measured on a 401 KB payload:
;
;     /SOLID lzma    198,868      what this uses
;     lzma           260,140      +31%, and all of it is the two DLLs' shared
;                                 code being compressed twice
;     /SOLID bzip2   256,714
;     /SOLID zlib    296,996
;
; /FINAL stops anything included later quietly downgrading that. The dictionary
; is headroom and nothing more - the default 8 MB is already twenty times the
; payload, and raising it to 64 changes the output by not one byte - but it
; costs only build-time memory, so it is set for whenever the payload grows.
SetCompressor /SOLID /FINAL lzma
SetCompressorDictSize 64
SetDatablockOptimize on

InstallDirRegKey HKCU "${REGKEY}" "InstallHome"
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION4}"
VIAddVersionKey "ProductName"     "${PRODUCT}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "FileDescription" "Installs the Declick and Dehum effects for VirtualDJ"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "MIT licence"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

; Text only, at every DPI - see note 3.
!define MUI_WELCOMEFINISHPAGE_NOBITMAP
!define MUI_UNWELCOMEFINISHPAGE_NOBITMAP
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "${PRODUCT} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT \
"Declick - autoregressive detect-and-interpolate declicker for shellac and \
vinyl transfers. It reads the song ahead of the play head, so the repair gets \
its lookahead and the deck stays in time: no delay at all.$\r$\n$\r$\n\
Dehum - finds continuous narrowband tones (mains hum, its harmonics, and the \
off-frequency drones on speed-corrected transfers) without being told the \
frequency, and cancels them with a tracking notch. Zero latency, plus an \
optional rumble high-pass.$\r$\n$\r$\n\
64 bit VirtualDJ 8.2 or later. Installed for you alone, without \
administrator rights.$\r$\n$\r$\n\
Close VirtualDJ if you are upgrading."

!define MUI_DIRECTORYPAGE_TEXT_TOP \
"Setup will install the plug-ins into the VirtualDJ folder below, under \
${PLUGDIR}.$\r$\n$\r$\n\
This should already be filled in correctly. Change it only if you run \
VirtualDJ from somewhere unusual - it is the folder holding your VirtualDJ \
database and settings, not the one holding VirtualDJ.exe."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "VirtualDJ folder"

!define MUI_FINISHPAGE_TITLE "Installed"
!define MUI_FINISHPAGE_TEXT \
"Start VirtualDJ. The plug-in folder is scanned at startup, so a running copy \
will not see them until it is restarted.$\r$\n$\r$\n\
The effects appear in Settings > Extensions > Effects, and in the effects list \
on a deck, as Declick and Dehum."
!define MUI_FINISHPAGE_NOREBOOTSUPPORT

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSEFILE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
;  Copy one plug-in, and do not pretend to have copied it if it was locked.
;
;  SetOverwrite try is what makes this possible: with the default "on", a File
;  that cannot be written aborts the whole installer with NSIS's own wording,
;  which does not mention the actual cause. Labels carry ${NAME} because the
;  macro is used twice in one section-less scope.
; ---------------------------------------------------------------------------
!macro InstallPlugin NAME
  SetOutPath "$INSTDIR\${PLUGDIR}"
  retry_${NAME}:
    ClearErrors
    SetOverwrite try
    File "${DIST}\${NAME}.dll"
    SetOverwrite on
    ${If} ${Errors}
      ; /SD IDCANCEL, not the default. MB_RETRYCANCEL's default button is
      ; Retry, and a silent install answers every box with its default - so
      ; without this, `setup.exe /S` over a running VirtualDJ would retry the
      ; same doomed copy forever, with no window to show for it.
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
        "Could not write ${NAME}.dll.$\r$\n$\r$\n\
         VirtualDJ is almost certainly running and has the plug-in loaded; it \
         keeps the file open until it exits. Close VirtualDJ completely, then \
         click Retry." \
        /SD IDCANCEL IDRETRY retry_${NAME}
      Abort "cancelled - ${NAME}.dll is in use by VirtualDJ"
    ${EndIf}
    DetailPrint "installed $INSTDIR\${PLUGDIR}\${NAME}.dll"
!macroend

; ---------------------------------------------------------------------------

Section "-prepare"
  SectionIn RO
  ; Recorded before the plug-in sections so the uninstaller exists even if one
  ; of them is cancelled halfway.
  ;
  ; The uninstaller does not go in the plug-in folder: VirtualDJ scans that
  ; directory and it should hold plug-ins and nothing else.
  SetOutPath "$LOCALAPPDATA\ShellacFilters\VirtualDJ"
  WriteUninstaller "$LOCALAPPDATA\ShellacFilters\VirtualDJ\uninstall.exe"

  WriteRegStr HKCU "${REGKEY}" "InstallHome" "$INSTDIR"
  WriteRegStr HKCU "${REGKEY}" "Version"     "${VERSION}"

  ; HKCU rather than HKLM, because the install is per-user and this key is not
  ; WOW64-redirected. It shows up in Programs and Features all the same.
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayName"     "${PRODUCT}"
  WriteRegStr HKCU "${UNINSTKEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr HKCU "${UNINSTKEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr HKCU "${UNINSTKEY}" "UninstallString" \
    '"$LOCALAPPDATA\ShellacFilters\VirtualDJ\uninstall.exe"'
  WriteRegStr HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR\${PLUGDIR}"
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "Declick" SEC_DECLICK
  !insertmacro InstallPlugin "Declick"
SectionEnd

Section "Dehum" SEC_DEHUM
  !insertmacro InstallPlugin "Dehum"
SectionEnd

Section "-size"
  ; Reported in Programs and Features. Measured rather than hardcoded so it
  ; stays right when only one component was chosen.
  ${GetSize} "$INSTDIR\${PLUGDIR}" "/M=*.dll /S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" $0
SectionEnd

LangString DESC_DECLICK ${LANG_ENGLISH} \
  "Declicker for shellac and vinyl transfers. Reads ahead, so it adds no delay."
LangString DESC_DEHUM ${LANG_ENGLISH} \
  "Hum and drone canceller. Finds the frequency itself and adds no delay."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DECLICK} $(DESC_DECLICK)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DEHUM}   $(DESC_DEHUM)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------------------

Function .onInit
  ; A 64 bit DLL is of no use to a 32 bit Windows, and there is no 32 bit
  ; VirtualDJ worth shipping for: 8.2 and later are 64 bit.
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP \
      "These plug-ins are 64 bit and this is a 32 bit Windows."
    Abort
  ${EndIf}

  ; If a previous install recorded a folder, InstallDirRegKey has already put
  ; it in $INSTDIR and it is left alone. Otherwise find VirtualDJ the way
  ; scripts\install.ps1 does: the 2023-and-later home first, then the 8-to-2021
  ; one, then give up and use the modern location, creating it.
  ${If} $INSTDIR == ""
    ${If} ${FileExists} "$LOCALAPPDATA\VirtualDJ\*.*"
      StrCpy $INSTDIR "$LOCALAPPDATA\VirtualDJ"
    ${ElseIf} ${FileExists} "$DOCUMENTS\VirtualDJ\*.*"
      StrCpy $INSTDIR "$DOCUMENTS\VirtualDJ"
    ${Else}
      StrCpy $INSTDIR "$LOCALAPPDATA\VirtualDJ"
      ; /SD IDOK: a silent install has no directory page to check on, and the
      ; modern per-user location is the right thing to create.
      MessageBox MB_OKCANCEL|MB_ICONINFORMATION \
        "No VirtualDJ folder was found.$\r$\n$\r$\n\
         If VirtualDJ is installed but has never been run, run it once so it \
         creates its folder, then try again. Otherwise you can carry on and \
         check the folder on the next page." \
        /SD IDOK IDOK +2
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------------------

Section "Uninstall"
  ReadRegStr $0 HKCU "${REGKEY}" "InstallHome"
  ${If} $0 == ""
    StrCpy $0 "$LOCALAPPDATA\VirtualDJ"
  ${EndIf}

  ; Named, never wildcarded: this folder belongs to VirtualDJ and may hold
  ; other people's effects.
  Delete "$0\${PLUGDIR}\Declick.dll"
  Delete "$0\${PLUGDIR}\Dehum.dll"

  ; Declick.ini and Dehum.ini are left where they are. VirtualDJ writes them
  ; next to the DLL and they hold the settings the user tuned, which are worth
  ; more than a tidy folder and cost nothing if the plug-ins never come back.
  ; They are also why the RMDirs below usually do nothing, which is fine.
  RMDir "$0\${PLUGDIR}"
  RMDir "$0\Plugins64"

  Delete "$LOCALAPPDATA\ShellacFilters\VirtualDJ\uninstall.exe"
  RMDir  "$LOCALAPPDATA\ShellacFilters\VirtualDJ"
  RMDir  "$LOCALAPPDATA\ShellacFilters"

  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "${REGKEY}"
  DeleteRegKey /ifempty HKCU "Software\ShellacFilters"
SectionEnd
