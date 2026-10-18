@echo off
rem RUNDEPTH n: the four CARDTEST tools at n bpp (called by RUNALL.BAT).
echo CARDTEST: %1 bpp
%W%\DEPTH /SET %1
if errorlevel 1 goto skip
%ST% %W%\GDICHK.EXE /Q /OUT=C:\CTRES\%1BPP
%ST% %W%\DDCHK.EXE /Q /OUT=C:\CTRES\%1BPP
%ST% %W%\D3DCHK.EXE /Q /OUT=C:\CTRES\%1BPP
%ST% %W%\GLCHK.EXE /Q /OUT=C:\CTRES\%1BPP
:skip
