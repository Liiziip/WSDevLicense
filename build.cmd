@echo off
rem Build the x86 reconstruction of arm.exe with MSVC 2019 (x86 / 32-bit).
rem Usage: build.cmd            -> build\arm_x86.exe
setlocal
if not defined VSDIR set "VSDIR=D:\Microsoft Visual Studio\2019\Professional"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" (
    echo ERROR: vcvars32.bat not found under %VSDIR%
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
if not exist build mkdir build
cl /nologo /W3 /TC /O2 /GS- /D_CRT_SECURE_NO_WARNINGS /Fo:build\ /Fe:build\arm_x86.exe ^
   src\main.c src\print.c src\file.c src\svc.c src\license.c src\rpc.c ^
   src\midl_blob.c src\wss_rpc_c.c src\bag.c src\serialize.c ^
   /link /SUBSYSTEM:CONSOLE /DYNAMICBASE /NXCOMPAT rpcrt4.lib advapi32.lib
if errorlevel 1 exit /b 1
echo.
echo built: build\arm_x86.exe
