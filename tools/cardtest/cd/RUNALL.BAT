@echo off
rem CARDTEST one-click run for Windows 95/98/Me/NT4/2000 (double-click it).
rem   RUNALL            every colour depth: GDICHK DDCHK D3DCHK GLCHK, then the extras
rem   RUNALL /SHUTDOWN  ... and shut Windows down at the end
rem   RUNALL /RESUME [/SHUTDOWN]  after a restart RUNGUI asked for (DirectX 7
rem                     install): finish RUNGUI, then the summary (RunOnce runs it)
rem Results: C:\CTRES\8BPP (16BPP ...), C:\CTRES\EXTRAS.LOG, C:\CTRES\SUMMARY.TXT
set CTCD=
for %%d in (D E F G H I J K) do if exist %%d:\CARDTEST.TAG set CTCD=%%d:
if "%CTCD%"=="" goto nocd
set W=%CTCD%\WIN
set ST=start /w
if "%OS%"=="Windows_NT" set ST=start /wait
set OFF=%1
if "%1"=="/RESUME" goto resume
if "%1"=="/resume" goto resume
echo CARDTEST: clearing C:\CTRES
if not exist C:\CTRES\NUL goto clean
if "%OS%"=="Windows_NT" rd /s /q C:\CTRES
if not "%OS%"=="Windows_NT" deltree /y C:\CTRES > nul
:clean
md C:\CTRES
%W%\DEPTH /SAVE C:\CTRES\DEPTH0.TXT
%W%\DEPTH /LIST > C:\CTRES\MODES.TXT
for %%b in (8 16 24 32) do call %CTCD%\RUNDEPTH.BAT %%b
%W%\DEPTH /RESTORE C:\CTRES\DEPTH0.TXT
rem If RUNGUI must restart Windows, RunOnce brings us back here; "start"
rem so RunOnce does not hold Explorer (and Direct3D) back while we run.
set CTRESUME=command.com /c start %CTCD%\RUNALL.BAT /RESUME %1
call %CTCD%\RUNXTRA.BAT
goto summary
:resume
set OFF=%2
echo CARDTEST: resuming after a restart
set CTRESUME=command.com /c start %CTCD%\RUNALL.BAT /RESUME %2
if exist %CTCD%\GUI\RUNGUI.BAT call %CTCD%\GUI\RUNGUI.BAT /RESUME
:summary
%W%\CTRUN /SUMMARY
if "%OFF%"=="/SHUTDOWN" goto off
if "%OFF%"=="/shutdown" goto off
echo CARDTEST done: C:\CTRES\SUMMARY.TXT
goto end
:off
%W%\DEPTH /SHUTDOWN
goto end
:nocd
echo CARDTEST CD not found (no ?:\CARDTEST.TAG)
:end
