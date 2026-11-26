[Setup]
AppId=JAM3S-Vocal-Engine
AppName=JAM3S Vocal Engine
AppVersion=2.0.0
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
WizardStyle=modern

[Files]
Source: "..\build\JAM3S_artefacts\Release\VST3\JAM3S Vocal Engine.vst3\*"; DestDir: "{app}\JAM3S Vocal Engine.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace
