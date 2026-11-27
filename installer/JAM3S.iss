[Setup]
AppId=JAM3S-Vocal-Engine
AppName=JAM3S Vocal Engine
AppVersion=2.4.0
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename=JAM3S_V2_Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern dark
DisableWelcomePage=no
WizardImageFile=branding\wizard.bmp
WizardSmallImageFile=branding\header.bmp
WizardImageBackColor=$101010
WizardSmallImageBackColor=$101010
SetupIconFile=branding\jam3s.ico

[Files]
Source: "..\build\JAM3S_artefacts\Release\VST3\JAM3S Vocal Engine.vst3\*"; DestDir: "{app}\JAM3S Vocal Engine.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace

[Messages]
WelcomeLabel1=JAM3S | CHROME INFERNO
WelcomeLabel2=Welcome to the JAM3S Vocal Engine 2.4 installation.%n%n40 signature vocal presets. Silver chrome. Red neon. Built for Windows VST3 hosts.%n%nClose FL Studio before installing. Click Next to continue.
FinishedHeadingLabel=JAM3S is ready
FinishedLabel=Chrome Inferno has been installed. Open FL Studio and rescan your VST3 plugins.
