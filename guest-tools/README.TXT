86Box Guest Tools 1.0
=====================

Mouse integration and a shared clipboard for 86Box-Next machines.

With the tools running, the mouse moves in and out of the machine's window
without being captured (no click to grab it, no key to let it go), and text
copied on the host can be pasted in the machine and the other way round.

First, in 86Box-Next
--------------------
Tools > Install 86Box Guest Tools puts this disk into the machine (the CD
if it has a CD-ROM drive, else the floppy), turning the tools on first if
they are off (the machine restarts then). Windows 95 and later start the
installer from the CD by themselves.

The tools are switched in Settings > Other peripherals > "86Box Guest
Tools"; Configure picks the features: mouse integration, and the
clipboard in either or both directions.

Windows 95, 98, ME, NT 4.0, 2000, XP and later
---------------------------------------------
Run 86BOXGT.EXE from this disk and answer Yes to install it: it copies
itself into the Windows directory and starts with Windows from then on.
Its icon in the notification area turns mouse integration and the shared
clipboard on and off.

    86BOXGT /install     install without asking
    86BOXGT /uninstall   stop it and remove it
    86BOXGT /quiet       no messages (with the above too)

Windows 3.1 and Windows for Workgroups 3.11
------------------------------------------
Run GTWIN.EXE from this disk (File > Run in Program Manager) and answer
Yes to install it: it copies itself into the Windows directory and adds
itself to the load= line of WIN.INI. It runs as an icon; its window menu
turns mouse integration and the shared clipboard on and off.

    GTWIN /install, /uninstall, /quiet   as above

No driver is installed: the agents talk to the emulator through two I/O
ports that 86Box-Next opens to programs while the tools are enabled.

Notes
-----
- Mouse integration needs the machine's own mouse (PS/2, serial or bus):
  the agent places the pointer, and the clicks and the wheel still come
  through that mouse.
- A program that confines the pointer (many games) gets the mouse to
  itself: the emulator then captures the mouse on a click, as usual.
- Clipboard: text only, up to 512K characters (16000 under Windows 3.x).
  Windows 3.x and 9x/ME keep text in an ANSI code page (Windows 3.x:
  Windows-1252), so characters outside it arrive as "?".
- Windows 3.x checks the pointer every 55 ms while it rests and keeps
  checking as fast as it can while it moves.
