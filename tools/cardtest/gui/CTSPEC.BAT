@echo off
rem CTSPEC.BAT - CARDTEST: run SPECviewperf 6.1.2 viewsets without Notepad.
rem Copied into the SPECviewperf folder by SPECVP.AGS. %1 = ALL for all six
rem viewsets; default: the three smaller ones (Dx, Light, AWadvs).
call run_viewset.bat Dx-06 DX results
call run_viewset.bat Light-04 Light results
call run_viewset.bat AWadvs-04 AWadvs results
if not "%1"=="ALL" goto sum
call run_viewset.bat Drv-07 DRV results
call run_viewset.bat MedMCAD-01 MedMCAD results
call run_viewset.bat ProCDRS-03 ProCDRS results
:sum
copy sum_results\DX\summary.txt C:\CTRES\GUI\SPECVP\DX.TXT
copy sum_results\Light\summary.txt C:\CTRES\GUI\SPECVP\LIGHT.TXT
copy sum_results\AWadvs\summary.txt C:\CTRES\GUI\SPECVP\AWADVS.TXT
if not "%1"=="ALL" goto end
copy sum_results\DRV\summary.txt C:\CTRES\GUI\SPECVP\DRV.TXT
copy sum_results\MedMCAD\summary.txt C:\CTRES\GUI\SPECVP\MEDMCAD.TXT
copy sum_results\ProCDRS\summary.txt C:\CTRES\GUI\SPECVP\PROCDRS.TXT
:end
