@echo off
call "D:\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars32.bat" >nul
"D:\Windows Kits\10\bin\10.0.17763.0\x86\midl.exe" /nologo /env win32 /ms_ext /c_ext  /acf "D:\DSH\workspace\tests\midl\gen\final.acf" /header "D:\DSH\workspace\tests\midl\gen\final.h" /cstub "D:\DSH\workspace\tests\midl\gen\final_c.c" /sstub "D:\DSH\workspace\tests\midl\gen\final_s.c" "D:\DSH\workspace\tests\midl\gen\final.idl"
