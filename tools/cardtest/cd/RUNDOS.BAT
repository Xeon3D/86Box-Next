@echo off
rem CARDTEST - DOS part. Run from the CD root (e.g. D:\RUNDOS).
rem Boot clean DOS (HIMEM only, no EMM386) so VBECHK can test the
rem linear frame buffer. /V adds the VISUAL screens (one key each).
echo CARDTEST DOS: results go to C:\CTRES
DOS\VGACHK %1
DOS\VBECHK
type C:\CTRES\VGACHK.LOG | find "CASE"
type C:\CTRES\VBECHK.LOG | find "CASE"
echo.
echo Benchmarks: THIRD\DOSBENCH\DOSBENCH.BAT (3DBench, PCPBench, Chris, Doom, Quake ...)
echo VESA:       THIRD\UNIVBE\VBETEST.EXE, THIRD\SPEEDSYS
