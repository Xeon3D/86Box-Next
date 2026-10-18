CARDTEST 1.0 - disc 2: the large third-party extras
===================================================

Root        3D WinBench 2000 CD as published (Ziff-Davis, freeware):
            3D WinBench 2000 1.0, WinBench 99 1.1, Audio WinBench 99,
            DirectX 7 - start Install.exe (or the autorun menu).
            3D WinBench 2000: run the "Quality" tests too - each 3D feature
            is drawn and compared with a reference picture (pass/fail).
            WinBench 99: Graphics WinMark plays back real applications' GDI
            calls - the widest 2D (GDI) coverage there is.
DX7SDK      DirectX 7.0a SDK binaries (no install needed):
            bin\dxutil\ddtest.exe        DirectDraw test (blits, flips, overlays)
            bin\dxutil\dxcapsviewer.exe  every DirectDraw/Direct3D caps bit
            samples\multimedia\d3dim\bin\*.exe   Direct3D samples: mipmap,
              filter, lights, fog (mfcfog), multitexture (mtexture), bump,
              envcube, stencil, w-buffer, vertex buffers, videotex ...
            samples\multimedia\ddraw\bin\*.exe   DirectDraw samples (ddex1-5,
              overlay "mosquito", flip, palettes)
            samples\multimedia\d3drm, d3dx       Retained Mode and D3DX samples
            (media files the samples load are in samples\multimedia\media)
FINALRLT    Final Reality 1.01 (Remedy, 1997): DirectX 5 3D benchmark with
            image-quality checks; SETUP.EXE installs it.
GLEAN       glean 1.0 for Win32: OpenGL conformance tests (blending,
            depth, stencil, pixel formats ...). bin\glean.exe -r RESULTDIR
            runs the tests, glean -c DIR1 DIR2 compares two runs.
SPECVP      SPECviewperf 6.1.2 (SPEC, 2000): OpenGL workstation viewsets
            (Advanced Visualizer, DesignReview, DX, Light, MedMCAD, ProCDRS)
            on Windows NT 4/2000 and 95/98; SETUP.EXE installs it. Slow on
            1998-era cards - pick single viewsets.

DCT53       Microsoft WHQL Display Compatibility Test 5.3 (January 2005) -
            the display tests of the logo program (GDI, DirectDraw, Direct3D
            DX6-DX9, OpenGL scenario, stress, power). ONLY for Windows XP
            SP2 / Server 2003 SP1 with DirectX 9.0c; log on as Administrator
            with a blank password; SETUP.EXE installs the test manager.
DCT130      WHQL DCT 1.30.0.300 (from HCT 9.5, Feb 2000): the logo display
            tests for Windows 95, 98, 2000 and NT 4.0 - GDI, DirectDraw,
            Direct3D, video. SETUP.EXE installs the DCT test manager.
DCT135      WHQL DCT 1.35.0.350 (from HCT 9.6, Aug 2000): Windows Me ONLY.

The full HCT kits are separate discs on the host (each nearly a CD):
  dl\cardtest-extra\hct91\HCT91.iso  HCT 9.1  Windows 98 + NT 4.0 (has the
       classic video tests: GDI scripts, DOS-box VGA tests vvd*.exe,
       OpenGL conformance oglcfm*.bat, DirectDraw tdd*.pro, MCD tests;
       hct\hwtest.exe to install)
  dl\cardtest-extra\hct95\HCT95.iso  HCT 9.5  Windows 2000 (and 9x)
  dl\cardtest-extra\hct96\HCT96.iso  HCT 9.6  Windows Me
The HCT 12.1 test specification (what each WHQL GDI/DirectDraw/D3D test
asserts) is on the host in cardtest\ref\hct12-graphics-spec.
