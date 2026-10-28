@echo off
rem Regenerate the MIDL client stub for the WSS interface (src\wss_rpc.idl).
rem Produces src\wss_rpc.h and src\wss_rpc_c.c, both of which are checked in so
rem that build.cmd does not need midl.  Run this only when the IDL changes.
setlocal
if not defined VSDIR set "VSDIR=D:\Microsoft Visual Studio\2019\Professional"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" (
    echo ERROR: vcvars32.bat not found under %VSDIR%
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
pushd "%~dp0.."
midl /nologo /env win32 /ms_ext /c_ext /Oicf ^
     /acf src\wss_rpc.acf /header src\wss_rpc.h /cstub src\wss_rpc_c.c ^
     /sstub "%TEMP%\wss_rpc_s.c" src\wss_rpc.idl
if errorlevel 1 (
    popd
    exit /b 1
)
popd
echo regenerated: src\wss_rpc.h  src\wss_rpc_c.c
