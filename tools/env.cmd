@echo off
rem MSVC libs/SDK from VS 2026 Build Tools, compilers from C:\Program Files\LLVM. Usage: env.cmd <command...>
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;N:\Projects\Monopoly\rexglue-sdk\out\install\win-amd64\bin;%PATH%"
%*
