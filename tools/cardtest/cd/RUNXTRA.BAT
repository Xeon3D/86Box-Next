@echo off
rem RUNXTRA: the scriptable extras (called by RUNALL.BAT). Games are
rem copied to C:\CTRUN, as they write logs and caches next to themselves.
echo CARDTEST: extras
if exist C:\CTRUN\GQ\GLQUAKE.EXE goto copied
md C:\CTRUN
xcopy %CTCD%\THIRD\GLQUAKE C:\CTRUN\GQ\ /e /i /q > nul
md C:\CTRUN\GQ\ID1\GLQUAKE
md C:\CTRUN\Q2
md C:\CTRUN\Q2\BASEQ2
copy %CTCD%\THIRD\QUAKE2\QUAKE2.EXE C:\CTRUN\Q2 > nul
copy %CTCD%\THIRD\QUAKE2\REF_*.DLL C:\CTRUN\Q2 > nul
:copied
%W%\CTRUN /JOBS %CTCD%\EXTRAS.TXT
rem Hook: the GUI benchmark runner (it also runs SPECviewperf).
if exist %CTCD%\GUI\RUNGUI.BAT call %CTCD%\GUI\RUNGUI.BAT
