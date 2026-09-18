@echo off
rem ==========================================================================
rem P3 优化：构建产物清理脚本（保留 Release 三架构 exe，安装包依赖它们）
rem 用法：双击或在仓库根目录执行 clean.cmd
rem ==========================================================================
setlocal

rem MSBuild 中间目录（obj 重命名后缀 + cppwinrt 生成头，重建时自动再生成）
if exist "AudioPla.2daaabdd" rd /s /q "AudioPla.2daaabdd"

rem Debug 配置产物（日常发布只用 Release；下次 Debug 构建自动再生成）
if exist "Debug"           rd /s /q "Debug"
if exist "x64\Debug"       rd /s /q "x64\Debug"
if exist "ARM64\Debug"     rd /s /q "ARM64\Debug"

rem 临时日志与构建残留
del /q debug.log restore.err restore.log build_*.log build_*.err 2>nul

rem 保留：Release / x64\Release / ARM64\Release（setup.iss 引用的三个 exe）
rem 保留：obj\（NuGet 还原目录，删除会触发全量还原）

echo Clean done. Release binaries kept.
endlocal
