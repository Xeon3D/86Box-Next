@echo off
rem CARDTEST - Windows part (95/98/ME/NT4/2000). Start it from anywhere
rem (Start, Run, D:\RUNWIN.BAT). Run once per colour depth (8, 16, 32 bpp);
rem each run overwrites C:\CTRES - let the host tool pull it in between.
set CTCD=
for %%d in (D E F G H I J K) do if exist %%d:\CARDTEST.TAG set CTCD=%%d:
if "%CTCD%"=="" goto nocd
if "%OS%"=="Windows_NT" goto nt
start /w %CTCD%\WIN\GDICHK.EXE %1
start /w %CTCD%\WIN\DDCHK.EXE %1
start /w %CTCD%\WIN\D3DCHK.EXE %1
start /w %CTCD%\WIN\GLCHK.EXE %1
goto done
:nt
start /wait %CTCD%\WIN\GDICHK.EXE %1
start /wait %CTCD%\WIN\DDCHK.EXE %1
start /wait %CTCD%\WIN\D3DCHK.EXE %1
start /wait %CTCD%\WIN\GLCHK.EXE %1
:done
echo CARDTEST Windows: results in C:\CTRES (*.LOG, one folder of BMPs per tool)
goto end
:nocd
echo CARDTEST CD not found (no ?:\CARDTEST.TAG)
:end
