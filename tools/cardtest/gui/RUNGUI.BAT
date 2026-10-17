@echo off
rem RUNGUI.BAT - CARDTEST unattended GUI benchmarks (AUTOGUI). Called by RUNALL
rem as its last stage, or on its own. Needs both CARDTEST discs visible
rem (two CD drives or the combined disc). Results: C:\CTRES\GUI\*.LOG + BMPs.
rem   RUNGUI           every script (old logs of these scripts are deleted)
rem   RUNGUI /RESUME   only the scripts that have no log yet
set CTCD=
for %%d in (D E F G H I J K) do if exist %%d:\CARDTEST.TAG set CTCD=%%d:
if "%CTCD%"=="" goto nocd
rem 3DM99 first: 3DMark 99 found no D3D accelerator once DirectX 7 was in
rem (Win98 SE, TNT2); 3DM2000 and the DX7 SDK samples need DirectX 7.
rem DX7 may restart Windows to finish the DirectX 7 install; RunOnce then
rem runs %CTRESUME% (RUNALL sets its own; on its own: RUNGUI /RESUME, through
rem "start" so that RunOnce does not hold Explorer back)
set CTLIST=3DM99 DX7 3DM2000 FINALR DX7SDK ZDWB SPECVP DCT130 HCT91
if "%CTRESUME%"=="" set CTRESUME=command.com /c start %CTCD%\RUNGUI.BAT /RESUME
if not exist C:\CTRES\GUI\NUL md C:\CTRES\GUI
rem AUTOGUI writes C:\CTRES\GUI\REBOOT.FLG before it restarts Windows: no
rem further script may start while Windows goes down
if exist C:\CTRES\GUI\REBOOT.FLG del C:\CTRES\GUI\REBOOT.FLG
if "%1"=="/RESUME" goto settle
if "%1"=="/resume" goto settle
for %%s in (%CTLIST%) do if exist C:\CTRES\GUI\%%s.LOG del C:\CTRES\GUI\%%s.LOG
goto run
:settle
rem started by RunOnce after a restart: let Explorer and the drivers come up
if "%OS%"=="Windows_NT" start /wait %CTCD%\WIN\AUTOGUI.EXE %CTCD%\GUI\SETTLE.AGS
if not "%OS%"=="Windows_NT" start /w %CTCD%\WIN\AUTOGUI.EXE %CTCD%\GUI\SETTLE.AGS
:run
if "%OS%"=="Windows_NT" goto nt
for %%s in (%CTLIST%) do if not exist C:\CTRES\GUI\REBOOT.FLG if not exist C:\CTRES\GUI\%%s.LOG if exist %CTCD%\GUI\%%s.AGS start /w %CTCD%\WIN\AUTOGUI.EXE %CTCD%\GUI\%%s.AGS
goto done
:nt
for %%s in (%CTLIST%) do if not exist C:\CTRES\GUI\REBOOT.FLG if not exist C:\CTRES\GUI\%%s.LOG if exist %CTCD%\GUI\%%s.AGS start /wait %CTCD%\WIN\AUTOGUI.EXE %CTCD%\GUI\%%s.AGS
:done
rem Windows is restarting: hold here (RUNALL must not carry on) until it
rem closes this box
:hold
if exist C:\CTRES\GUI\REBOOT.FLG goto hold
echo CARDTEST GUI benchmarks done: C:\CTRES\GUI
goto end
:nocd
echo CARDTEST CD not found
:end
