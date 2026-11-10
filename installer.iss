; Studious 安装包脚本（Inno Setup 6）
;
; 用法：
;   ISCC.exe /DMyAppVersion=0.1.0 installer.iss
;   （package.ps1 -MakeInstaller 会自动取 version.json 里的版本号并调用）
;
; 产物：dist\Studious-<版本>-setup.exe
;
; 约定：
;   - 安装内容来自 dist\Studious-<版本>-win64\（package.ps1 的产物），可用 /DStageDir= 覆盖
;   - 默认按用户安装（不需要管理员）；有管理员权限的人可在向导里选“所有用户”
;   - data\ 不覆盖：升级安装不会动用户的日志、记录、已下载模型
;   - data\assets\models\ocr\run 只在缺失时补（首次安装即带视觉模型）

#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef StageDir
  #define StageDir "dist\Studious-" + MyAppVersion + "-win64"
#endif

#define MyAppName "Studious"
#define MyAppPublisher "hua-2006"
#define MyAppUrl "https://github.com/hua-2006/Studious"
#define MyAppExeName "studious.exe"

[Setup]
; AppId 是“这个软件”的身份，升级/卸载都靠它，绝对不能改
AppId={{4F1C9A62-48D3-4E7B-9C55-8A2B6D3F0E17}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppUrl}
AppSupportURL={#MyAppUrl}/issues
AppUpdatesURL={#MyAppUrl}/releases
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} 安装程序

DefaultDirName={autopf}\Studious
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; 默认按用户装（{localappdata}\Programs\Studious），不弹 UAC；
; 想装到 Program Files 的人在向导第一页选“为所有用户安装”
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

OutputDir=dist
OutputBaseFilename=Studious-{#MyAppVersion}-setup
SetupIconFile=frontend\resources\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes

[Languages]
Name: "zh"; MessagesFile: "installer\ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked

[Files]
; 主程序 + Qt 运行库 + qml/ + 插件 + onnxruntime.dll + version.json + studious_*.qm
; data\ 一律不进（运行期数据，属于用户）
Source: "{#StageDir}\*"; DestDir: "{app}"; \
    Excludes: "data\*,*.pdb,*.ilk"; \
    Flags: ignoreversion recursesubdirs createallsubdirs

; 视觉（OCR）模型：只有首次安装、或用户删了才补；升级时 already exists 就跳过
Source: "{#StageDir}\data\assets\models\ocr\run\*"; DestDir: "{app}\data\assets\models\ocr\run"; \
    Flags: onlyifdoesntexist skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即启动 {#MyAppName}"; \
    Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 只清运行期垃圾：日志、缓存库。模型、记录、设置（HKCU）留给用户
Type: filesandordirs; Name: "{app}\data\logs"
Type: files; Name: "{app}\data\cache.db"
Type: filesandordirs; Name: "{app}\data\assets\models\ocr\run"
