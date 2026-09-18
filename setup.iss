; Inno Setup Script for Audio Bridge
; ==============================================================
; 架构策略：一个 Setup 包包含 x86 / x64 / ARM64 三个二进制，
;           安装时自动检测当前系统架构并三选一安装。
; 最终安装文件名：{app}\Audio_Bridge.exe（三架构统一命名）
; 因此所有 [Icons]/[Registry]/[Run]/UninstallDisplayIcon 全是静态引用，
; 彻底避免编译期 {#...} 与运行期 {code:...} 的混淆风险。
; ==============================================================

; ==============================================================
; 版本号单点定义：[Setup] 与 [Code] 的 installed.arch.txt 均引用此宏，
; 发布新版本时只改这一行（rc/manifest 的版本需同步手动更新）
; ==============================================================
#define MyAppVersion "1.1.4"

[Setup]
AppId={{A5E8B3D4-1C7E-4F8A-9B2C-1D3E4F5A6B7C}
AppName=Audio Bridge
AppVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}.0
AppPublisher=Dylan
DefaultDirName={autopf}\Audio_Bridge
DefaultGroupName=Audio Bridge
AllowNoIcons=yes
LicenseFile=LICENSE
Compression=lzma2
SolidCompression=yes
OutputDir=Output
OutputBaseFilename=Audio_Bridge_Setup
; 安装包本身（向导窗口、任务栏、Setup.exe 图标）使用项目根目录下的 ICO
SetupIconFile=AudioBridge.ico
UninstallDisplayIcon={app}\Audio_Bridge.exe,0
UninstallDisplayName=Audio Bridge
WizardStyle=modern
; Inno Setup 6.3+ 新架构标识符（消除弃用警告）
ArchitecturesAllowed=x86os x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 升级覆盖：已安装时默认原路径
DisableDirPage=auto
DisableProgramGroupPage=auto
; 每次安装强制覆盖旧版 exe，避免用户反复卸载/安装
PrivilegesRequired=lowest
; 关闭 Restart Manager 自动关程序：本程序是常驻托盘应用（隐藏窗口），
; RM 通常识别不到也无法关闭它，还会弹"需要关闭的程序"对话框干扰卸载/升级；
; 改用 [Code] 段的 StopRunningApp 统一处理：先请求优雅退出，超时再强制结束。
; 若强杀后进程仍被内核钉住（僵尸进程），不再中止卸载 / 安装，而是改为
; "改名让位 + RunOnce 收尾"，保证卸载始终能稳定完成（详见
; InitializeUninstall / CurUninstallStepChanged / PrepareToInstall）。
; 收尾用 wscript.exe 执行自删除的 VBScript（而非 cmd 拼接），登录时完全静默、不闪黑框。
CloseApplications=no
RestartApplications=no

[Languages]
Name: "chinese"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startup"; Description: "Start on Boot"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

; ==========================================================
; [Files] 三架构来源直接引用各平台 MSBuild 的实际输出目录
;   （Win32 → Release\，x64 → x64\Release\，ARM64 → ARM64\Release\），
;   无需构建后手动复制到统一目录，避免打包到过期产物。
;   DestName 强制统一为 Audio_Bridge.exe
; ==========================================================
[Files]
; x86 (32-bit Windows)：Win32 平台输出目录为项目根的 Release\
Source: "Release\Audio_Bridge32.exe"; \
    DestDir: "{app}"; DestName: "Audio_Bridge.exe"; \
    Flags: ignoreversion; Check: IsX86

; x64 (64-bit x86/x64 Windows)：x64 平台输出目录为 x64\Release\
Source: "x64\Release\Audio_Bridge64.exe"; \
    DestDir: "{app}"; DestName: "Audio_Bridge.exe"; \
    Flags: ignoreversion; Check: IsX64

; ARM64 (Snapdragon / Surface Pro X 等)：ARM64 平台输出目录为 ARM64\Release\
Source: "ARM64\Release\Audio_BridgeARM64.exe"; \
    DestDir: "{app}"; DestName: "Audio_Bridge.exe"; \
    Flags: ignoreversion; Check: IsARM64

; ==========================================================
; 所有快捷方式、注册表全写死 Audio_Bridge.exe
; ==========================================================
[Icons]
; 开始菜单快捷方式必须设置 AppUserModelID=AudioBridge，与 C++ 中
; ToastNotificationManager::CreateToastNotifier(L"AudioBridge") 使用的 AUMID 一致。
; Windows 会在开始菜单中查找 AUMID 匹配的快捷方式，并用其图标（exe 的 ICO 资源）
; 作为 Toast 通知头部的应用图标；缺失此项时 Toast 头部不会显示图标。
Name: "{group}\Audio Bridge"; Filename: "{app}\Audio_Bridge.exe"; AppUserModelID: "AudioBridge"
Name: "{group}\{cm:UninstallProgram,Audio Bridge}"; Filename: "{uninstallexe}"
; 桌面快捷方式也设置相同 AUMID，保持一致性（Toast 图标解析以开始菜单为准）
Name: "{userdesktop}\Audio Bridge"; Filename: "{app}\Audio_Bridge.exe"; Tasks: desktopicon; AppUserModelID: "AudioBridge"
Name: "{userstartup}\Audio Bridge"; Filename: "{app}\Audio_Bridge.exe"; Parameters: "/startup"; Tasks: startup

[Registry]
Root: HKCU; \
    Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "Audio_Bridge"; \
    ValueData: """{app}\Audio_Bridge.exe"" /startup"; \
    Tasks: startup; Flags: uninsdeletevalue
Root: HKCU; \
    Subkey: "SOFTWARE\Classes\AppUserModelId\AudioBridge"; \
    ValueType: string; ValueName: ""; ValueData: "Audio Bridge"

[Run]
Filename: "{app}\Audio_Bridge.exe"; Description: "{cm:LaunchProgram,Audio Bridge}"; Flags: nowait postinstall skipifsilent

; ==========================================================
; [UninstallDelete]：清理安装/运行时在 {app} 下生成的"孤儿文件"
;   这些文件不在 [Files] 清单中，Inno 默认不会删，导致卸载后 {app}
;   目录非空、文件夹删不干净。
;   仅精确列出已知文件名，绝不使用通配/递归删除，避免误删用户数据。
; ==========================================================
[UninstallDelete]
; 由 [Code] CurStepChanged(ssPostInstall) 写入的安装标记
Type: files; Name: "{app}\installed.arch.txt"
; 旧版本（配置存 exe 同目录时期）遗留的 Audio_Bridge.json / .bak
Type: files; Name: "{app}\Audio_Bridge.json"
Type: files; Name: "{app}\Audio_Bridge.json.bak"
; 仅当 {app} 已变空时才移除该空目录（dirifempty 语义）；
; 若目录内仍留有文件（用户数据/日志等），则自动跳过，绝不递归删除
Type: dirifempty; Name: "{app}"

; ==========================================================
; [Code]：架构判断（IsX86/IsX64/IsARM64）、安装标记写入，
;         以及"卸载/覆盖前结束正在运行的程序"的统一处理。
; ==========================================================
[Code]
// ==========================================================
// 卸载 / 覆盖升级前自动结束正在运行的程序
// ----------------------------------------------------------
// 背景：Audio_Bridge.exe 常驻托盘运行，占用自身文件句柄，
//       导致卸载或覆盖安装时 Inno 无法删除 / 替换该 exe。
// 策略：先通过 WM_APP_QUITREQUEST 请求程序优雅退出
//       （关闭蓝牙连接、保存设置、移除托盘图标、释放 Mutex），
//       轮询等待其自行退出；若超时仍在运行，则 taskkill /F 强制结束。
// 收尾：若强杀后进程仍被内核钉住（僵尸进程，用户态无法结束），
//       【绝不中止卸载 / 安装】，改为"改名让位 + 登录后清理"：
//       exe 改名 .old 让出路径 → 卸载照常完成 → HKCU RunOnce 在下次登录时删除残留。
// 注意：以下两个常量必须与 C++ 源码保持一致：
//       APP_MUTEX_NAME      ↔ AudioBridge.cpp 的
//                             CreateMutexW(..., L"AudioBridge_SingleInstance")
//       WM_APP_QUITREQUEST  ↔ AudioBridge.h 的
//                             WM_APP_QUITREQUEST = WM_APP + 0x103
// ==========================================================
const
  APP_MUTEX_NAME = 'AudioBridge_SingleInstance';
  WM_APP_QUITREQUEST = $8103;  // WM_APP(0x8000) + 0x103

var
  // StopRunningApp() 的结论缓存：True 表示强杀后进程仍被内核钉死（僵尸进程），
  // 需要执行"改名让位"并登记登录后的自动清理。卸载与安装两条路径共用。
  AppWasPinned: Boolean;
  // 让位改名的实际结果：非空 = 已成功改名到该路径（登录后自动清理）；
  // 空串 = 未发生让位。用于让最终提示如实反映"清理是否已自动安排"，不说空话。
  LetAsidePath: String;

// lpWindowName 传 0 表示 NULL（按窗口类名匹配即可；窗口标题会随语言本地化，
// 不可用作匹配依据）。PascalScript 无 Pointer 类型，故此处用 Integer 承载 NULL。
function FindWindowW(lpClassName: String; lpWindowName: Integer): HWND;
  external 'FindWindowW@user32.dll stdcall';

function PostMessageW(hWnd: HWND; Msg: UINT; wParam: Integer; lParam: Integer): Boolean;
  external 'PostMessageW@user32.dll stdcall';

// 注：命名为 KernelSleep 而非 Sleep，避免与 Inno Setup 内置的 Sleep 函数重名
procedure KernelSleep(Milliseconds: Cardinal);
  external 'Sleep@kernel32.dll stdcall';

// 用原生 MoveFileW 而非脚本内置的 RenameFile：内置实现可能带 MOVEFILE_COPY_ALLOWED
//   回退（同卷重命名失败时改走"复制 + 删除"），而被内核钉住的 exe【无法被复制】，
//   会把本来能成功的"改名"退化成彻底失败。原生 MoveFileW 只做重命名语义，
//   与实测结论（运行中的 exe 可改名 = OK）严格一致。
function MoveFileW(lpExistingFileName, lpNewFileName: String): Boolean;
  external 'MoveFileW@kernel32.dll stdcall';

// 检测 Audio_Bridge.exe 进程是否仍存在于系统（不依赖 Mutex）。
// 【为什么需要双通道检测】程序退出路径若卡死在内核态（如蓝牙驱动 IRP 未完成），
// 进程会成为"僵尸"：主进程已终止、Mutex 已被系统清理，但 exe 文件句柄仍被
// 卡死的内核线程锁定 —— 此时仅查 Mutex 会漏判，卸载器会在"程序未运行"的
// 假象下直接删文件，结果 exe 删不掉，留下"未删除干净"的残留目录。
// 【为什么必须解析输出而非看退出码】实测 tasklist 无论是否匹配到进程，
// 退出码恒为 0（无匹配时仅打印一行 INFO），依据退出码会得到恒真的错误结论，
// 必须把输出重定向到临时文件后按行匹配进程名。
function IsProcessAlive(): Boolean;
var
  TmpFile: String;
  Lines: TArrayOfString;
  I, ResultCode: Integer;
begin
  Result := False;
  TmpFile := ExpandConstant('{tmp}\AudioBridgeProcCheck.txt');

  if FileExists(TmpFile) then
    DeleteFile(TmpFile);

  // 不带过滤条件列出全部进程（/NH 去掉表头），再在 Pascal 侧精确匹配，
  // 避免"任务名过滤引号"与 cmd /C 的引号解析规则冲突
  if Exec(ExpandConstant('{cmd}'),
      '/C tasklist /FO CSV /NH > "' + TmpFile + '"',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    if LoadStringsFromFile(TmpFile, Lines) then
    begin
      for I := 0 to GetArrayLength(Lines) - 1 do
      begin
        // CSV 中进程名带引号且为完整名称；连后引号一起匹配可避免误命中
        // 同前缀进程（如 "Audio_Bridge_Setup.exe"）
        if Pos('"Audio_Bridge.exe"', Lines[I]) > 0 then
        begin
          Result := True;
          Break;
        end;
      end;
    end;
  end;

  if FileExists(TmpFile) then
    DeleteFile(TmpFile);
end;

// 结束正在运行的程序（尽力而为，不再作为流程闸门）。
// 返回 True  → 程序已确认退出（或本来就没运行），exe 可直接删除 / 覆盖。
// 返回 False → 优雅退出与强杀都未能结束进程（典型：僵尸进程，见上）。
//              调用方【不再中止流程】，而是记录到 AppWasPinned，改由
//              「改名让位 + 登录后清理」收尾，保证卸载 / 安装能稳定走完。
// 【性能修复】旧实现在轮询循环里每轮都调 IsProcessAlive() —— 每次调用都要
//   启动 cmd.exe + tasklist 并等其写完输出文件，单次就秒级；僵尸进程场景
//   下 IsProcessAlive 恒为 True，20 + 8 轮轮询叠加起来阻塞 30 秒以上。
//   新实现：轮询只查 Mutex（进程一终止系统立即回收 Mutex，查询零开销），
//   tasklist 只在关键节点做单次终验 —— 僵尸场景 30s+ 降到 ~8s，
//   正常退出 ~3s，未运行 ~1s。
function StopRunningApp(): Boolean;
var
  Hwnd: HWND;
  I, ResultCode: Integer;
  MutexGone: Boolean;
begin
  Result := False;

  // 双通道检测：Mutex 持有 或 进程存活，任一命中即视为"在运行"
  // （短路求值：Mutex 命中时不再花 tasklist 的开销）
  if not CheckForMutexes(APP_MUTEX_NAME) and not IsProcessAlive() then
  begin
    Result := True;
    Exit;
  end;

  // 1) 请求优雅退出（按窗口类名查找，标题会随语言本地化故不能用作匹配）
  Hwnd := FindWindowW('AudioBridge', 0);
  if Hwnd <> 0 then
    PostMessageW(Hwnd, WM_APP_QUITREQUEST, 0, 0);

  // 2) 最多等待 5 秒（20 × 250ms）：程序会关闭连接、保存设置并自行退出。
  //    只查 Mutex：进程终止（含卡死后被系统标记终止）时 Mutex 必被回收；
  //    Mutex 仍在 = 进程整体还活着，tasklist 查了也一样，不必反复启动它。
  MutexGone := False;
  for I := 1 to 20 do
  begin
    KernelSleep(250);
    if not CheckForMutexes(APP_MUTEX_NAME) then
    begin
      MutexGone := True;
      Break;
    end;
  end;

  // 3) Mutex 已回收：进程已终止（正常退出，或已成僵尸）。
  //    内核释放 exe 文件句柄有延迟，缓 1 秒后做【单次】tasklist 终验
  //    （仅查 Mutex 会漏判僵尸进程 —— Mutex 已回收但 exe 句柄仍被
  //    卡死线程锁定，直接删文件会触发"有部分内容未能被删除"）。
  if MutexGone then
  begin
    KernelSleep(1000);
    if not IsProcessAlive() then
    begin
      Result := True;  // 优雅退出成功
      Exit;
    end;
    // Mutex 没了但进程仍列于 tasklist = 僵尸进程。
    // 不再长轮询（等不出结果），落到下方强杀做最后一次尝试。
  end;

  // 4) 优雅退出超时（Mutex 仍在）或已判僵尸 → 强制结束进程
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM Audio_Bridge.exe',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

  // 5) 强杀后【单次】复核：驱动 IRP 取消和句柄释放需更久，等 2 秒
  //    （进程被强制终止后内核清理句柄比优雅退出更慢）
  KernelSleep(2000);
  if not IsProcessAlive() then
    Result := True;
  // 仍在运行：强杀后进程仍存活 = 僵尸进程（内核级锁定，用户态无法清理）。
  // 如实上报 False，调用方转用「改名让位 + 登录后清理」，不中止流程。
end;

// 僵尸进程"改名让位"：把被内核锁定、无法删除的 exe 改名到 .old。
// 【原理】Windows 锁定的是"文件对象"而非路径：进程映像一旦被加载，
//   该文件即不可删除（实测：删除运行中的重命名文件 → 访问被拒绝），
//   但【仍然可以改名】（实测：RENAME_RUNNING_EXE = OK）。
//   改名后原路径立即空闲，Inno 即可照常删除其余文件、走完卸载流程。
// 返回改名后的残留文件完整路径；未发生让位（无需让位或改名失败）时返回空串。
function RenameAsideLockedExe(): String;
var
  Src, Dst: String;
begin
  Result := '';
  Src := ExpandConstant('{app}\Audio_Bridge.exe');
  Dst := Src + '.old';

  if not FileExists(Src) then
    Exit;

  // 目标名被上次的残留占用：先尝试清掉；连它都删不掉说明同样处于锁定，
  // 本次放弃改名（不改名也不会更糟，登录后的 RunOnce 仍会清理残留）
  if FileExists(Dst) and not DeleteFile(Dst) then
    Exit;

  if MoveFileW(Src, Dst) then
    Result := Dst;
end;

// 生成"登录后清理"用的 VBScript 内容。
// 【为什么不用 cmd 的 del/rmdir 拼接】RunOnce 里的 cmd.exe 是控制台程序，
//   登录时会被系统分配一个控制台窗口 —— 用户会看到一个黑框一闪而过。
//   wscript.exe 是 GUI 子系统程序，不分配控制台，配合 //B 完全静默。
// 【为什么用 VBS 而非直接 exec】VBS 的 DeleteFile 能顺带处理只读属性，
//   且脚本执行完毕能删除自己（WSH 启动时已把脚本读入内存，不占用文件句柄）。
// 【陷阱 · 实测确认】FSO 的 DeleteFolder 是【递归删除】：第二个 force 参数
//   只表示"是否连只读项一起删"，并不提供 rmdir 那种"非空则拒删"的保护 ——
//   直接调用会连同用户自己放进 {app} 的文件一起抹掉。所以这里必须先判定
//   目录确实为空，再删，语义才真正等价于原来的 rmdir。
// 【为什么先切当前目录】wscript 的当前目录若恰好是 {app}，该目录会被自己的
//   cwd 钉住而删不掉，先把 cwd 挪到 %SystemRoot% 再删。
function BuildCleanupScript(const LeftoverPath: String; RemoveAppDir: Boolean): String;
var
  S: String;
begin
  S := 'Option Explicit' + #13#10
     + 'Dim fso, sh, AppDir' + #13#10
     + 'Set fso = CreateObject("Scripting.FileSystemObject")' + #13#10
     + 'Set sh = CreateObject("WScript.Shell")' + #13#10
     + 'On Error Resume Next' + #13#10
     + 'sh.CurrentDirectory = sh.ExpandEnvironmentStrings("%SystemRoot%")' + #13#10
     + 'fso.DeleteFile "' + LeftoverPath + '", True' + #13#10;

  if RemoveAppDir then
  begin
    S := S
       + 'fso.DeleteFile "' + ExpandConstant('{app}\Audio_Bridge.exe') + '", True' + #13#10
       + 'Set AppDir = fso.GetFolder("' + ExpandConstant('{app}') + '")' + #13#10
       + 'If AppDir.Files.Count = 0 And AppDir.SubFolders.Count = 0 Then' + #13#10
       + '  fso.DeleteFolder "' + ExpandConstant('{app}') + '", True' + #13#10
       + 'End If' + #13#10;
  end;

  // 最后删除脚本自身：RunOnce 项本身由系统在执行后自动移除，不留痕迹
  S := S + 'fso.DeleteFile WScript.ScriptFullName, True' + #13#10;

  Result := S;
end;

// 登记"下次登录时"的残留清理：写 HKCU\...\RunOnce，登录时执行一次后自动移除该项。
// 【为什么必须借道 RunOnce】本项目 PrivilegesRequired=lowest，没有管理员权限：
//   MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT) 需要写
//   HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations，
//   实测被系统拒绝（SESSION_MGR_WRITE = DENIED，不允许所请求的注册表访问权），
//   所以只能改用用户态可写的 HKCU RunOnce 作为"重启后收尾"的入口。
//   重启后僵尸进程已随关机销毁，内核锁定的文件句柄随之释放，普通权限即可删除。
// 【执行方式】wscript.exe //B //Nologo：//B 批处理模式，脚本内的任何错误都只记录、
//   不弹错误对话框（否则登录时会突然弹窗，比闪黑框更糟）。
// 【兜底】脚本文件若写入失败，退回原来的 cmd 拼接方式：宁可闪一次黑框，
//   也不能让清理彻底不发生。
// RemoveAppDir 仅在卸载路径为 True：那才是真正要清干净的场景；
//   安装路径【绝不能】删 exe 和目录 —— 同一路径上放的是刚装好的新文件。
procedure RegisterLogonCleanup(const LeftoverPath: String; RemoveAppDir: Boolean);
var
  CleanupDir, VbsPath, Cmd: String;
  ScriptOk: Boolean;
begin
  // 放在用户临时目录下：可写、跨重启保留，且不在 {app} 内 ——
  // 不干扰卸载时 (dirifempty) 对 {app} 的空目录判定
  CleanupDir := ExpandConstant('{%TEMP}');
  if (CleanupDir = '') or (not DirExists(CleanupDir)) then
    CleanupDir := ExpandConstant('{localappdata}') + '\Temp';
  if not DirExists(CleanupDir) then
    ForceDirectories(CleanupDir);

  VbsPath := CleanupDir + '\Audio_Bridge_Cleanup.vbs';
  ScriptOk := SaveStringToFile(VbsPath, BuildCleanupScript(LeftoverPath, RemoveAppDir), False);

  if ScriptOk and FileExists(VbsPath) then
  begin
    Cmd := '"' + ExpandConstant('{sys}\wscript.exe') + '" //B //Nologo "' + VbsPath + '"';
    Log('Audio Bridge: 已登记登录后静默清理 (wscript) -> ' + VbsPath);
  end
  else
  begin
    Cmd := ExpandConstant('{cmd}') + ' /C del /F /Q "' + LeftoverPath + '"';
    if RemoveAppDir then
      Cmd := Cmd
           + ' & del /F /Q "' + ExpandConstant('{app}\Audio_Bridge.exe') + '"'
           + ' & rmdir "' + ExpandConstant('{app}') + '"';
    Log('Audio Bridge: 清理脚本写入失败，回退 cmd 方式');
  end;

  RegWriteStringValue(HKCU,
    'Software\Microsoft\Windows\CurrentVersion\RunOnce',
    'Audio_Bridge_Cleanup', Cmd);
end;

// 卸载开始时触发（任何卸载对话框显示之前）。
// 【关键】这里不做任何耗时操作，恒返回 True：
//   1) 此前在僵尸进程场景下返回 False 会"中止整个卸载"—— 用户看到的
//      正是"卸载已终止 / 未删除任何文件"的报错弹窗，且一个文件都删不掉。
//      现在恒 True：结束不了也要继续卸载，靠 CurUninstallStepChanged 的
//      "改名让位 + 登录后清理"收尾。
//   2) 【时序修复】旧版还在此处调用 StopRunningApp() —— 该函数在本事件
//      里执行时【没有任何卸载器界面存在】，僵尸进程场景下阻塞 30 秒以上，
//      用户以为卸载器没启动而反复点击，后续实例被 Inno 的卸载互斥立即
//      拒绝退出 —— 正是"卸载窗口反复弹出又消失、半天才出现卸载窗口"
//      的元凶。进程处理已移至 CurUninstallStepChanged(usUninstall)
//      （用户确认卸载之后）：确认框立即弹出，等待期间卸载器窗口常驻。
function InitializeUninstall(): Boolean;
begin
  Result := True;
end;

// 卸载过程中触发。
//   usUninstall     ：早于 Inno 删除文件。此刻用户已确认卸载（卸载器窗口
//                     常驻可见），在这里结束运行中的程序并执行"改名让位"；
//                     让位后原路径空闲，Inno 的删除与目录清理才不会报错。
//   usPostUninstall ：卸载已结束，只给一条【说明性】提示 —— 不再用报错语气，
//                     不再让用户以为卸载失败，也不再有"请重启后重试"的中止逻辑。
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    // 时序说明见 InitializeUninstall：进程处理放在用户确认之后
    AppWasPinned := not StopRunningApp();

    if AppWasPinned then
    begin
      // StopRunningApp 返回 False = 进程未能结束（僵尸）→ 改名让位
      LetAsidePath := RenameAsideLockedExe();
      if LetAsidePath <> '' then
      begin
        Log('Audio Bridge: 进程被内核钉住，已让位改名 -> ' + LetAsidePath);
        RegisterLogonCleanup(LetAsidePath, True);
      end
      else
        Log('Audio Bridge: 进程被内核钉住，exe 让位改名失败');
    end;
  end
  else if CurUninstallStep = usPostUninstall then
  begin
    // 提示分两种，措辞与实际状态严格对应，不含"已自动清理"之类的空话
    if AppWasPinned and (LetAsidePath <> '') then
      MsgBox('卸载已完成。' + #13#10#13#10
             + '过程中检测到 Audio Bridge 进程被蓝牙驱动卡死（已被系统底层锁定），'
             + '它的程序文件无法当场直接删除。' + #13#10#13#10
             + '程序文件已改名暂存，因此安装目录可能在下次开机前仍然存在 —— '
             + '这是预期现象，不是卸载失败。' + #13#10#13#10
             + '重启（或下次登录）后残留会自动清除，无需任何手动操作；'
             + '也不必再次运行卸载程序 —— 它已随本次卸载一并移除。',
             mbInformation, MB_OK)
    else if AppWasPinned then
      MsgBox('卸载已完成。' + #13#10#13#10
             + '过程中检测到 Audio Bridge 进程被蓝牙驱动卡死（已被系统底层锁定），'
             + '且它的程序文件改名让位也未成功，因此程序文件可能仍留在安装目录。'
             + #13#10#13#10
             + '这不是权限问题：重启电脑后锁定即会解除，届时手动删除该安装目录'
             + '即可彻底清除（卸载程序本身已随本次卸载移除，无需再次运行）。',
             mbInformation, MB_OK);
  end;
end;

// 安装 / 升级开始时触发（复制文件之前）。
// 【关键】不再因为"结束不了进程"返回错误字符串中止安装：改为"改名让位"，
//   让安装照常完成；被锁定的旧 exe 改名后交由登录后的 RunOnce 清理。
//   （旧的实现会中止安装并弹出"请重启电脑后重试安装"，与新的删除策略相悖。）
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Leftover: String;
begin
  Result := '';
  AppWasPinned := not StopRunningApp();

  if AppWasPinned then
  begin
    // StopRunningApp 返回 False = 进程未能结束（僵尸）→ 改名让位
    Leftover := RenameAsideLockedExe();
    if Leftover <> '' then
    begin
      Log('Audio Bridge: 进程被内核钉住，已让位改名 -> ' + Leftover);
      RegisterLogonCleanup(Leftover, False);
    end
    else
      Log('Audio Bridge: 进程被内核钉住，exe 让位改名失败');
  end;
end;

function IsX86: Boolean;
begin
  Result := not IsWin64 and (ProcessorArchitecture = paX86);
end;

function IsX64: Boolean;
begin
  Result := IsWin64 and (ProcessorArchitecture = paX64);
end;

function IsARM64: Boolean;
begin
  Result := (ProcessorArchitecture = paARM64);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Arch: String;
  Data: AnsiString;
begin
  if CurStep = ssPostInstall then
  begin
    if IsARM64 then Arch := 'ARM64'
    else if IsX64 then Arch := 'x64'
    else                Arch := 'x86';

    Data := 'Architecture: ' + Arch + #13#10
          + 'InstallDir:   ' + ExpandConstant('{app}') + #13#10
          + 'AppVersion:   {#MyAppVersion}' + #13#10
          + 'InstallDate:  ' + GetDateTimeString('yyyy-mm-dd hh:nn:ss', '-', ':') + #13#10;

    SaveStringToFile(ExpandConstant('{app}\installed.arch.txt'), Data, False);
  end;
end;
