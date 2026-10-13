86Box Guest Tools 1.0
=====================

Mouse integration and a shared clipboard for 86Box-Next machines.

With the tools running, the mouse moves in and out of the machine's window
without being captured (no click to grab it, no key to let it go), and text
copied on the host can be pasted in the machine and the other way round.

First, in 86Box-Next
--------------------
Settings > Other peripherals: tick "86Box Guest Tools". Configure picks
the features: mouse integration, and the clipboard in either or both
directions. The machine restarts.

Windows 95, 98, ME, NT 4.0, 2000, XP and later
---------------------------------------------
Run 86BOXGT.EXE from this disk and answer Yes to install it: it copies
itself into the Windows directory and starts with Windows from then on.
Its icon in the notification area turns mouse integration and the shared
clipboard on and off.

    86BOXGT /install     install without asking
    86BOXGT /uninstall   stop it and remove it
    86BOXGT /quiet       no messages (with the above too)

No driver is installed: the agent talks to the emulator through two I/O
ports that 86Box-Next opens to programs while the tools are enabled.

Notes
-----
- Mouse integration needs the machine's own mouse (PS/2, serial or bus):
  the agent places the pointer, and the clicks and the wheel still come
  through that mouse.
- A program that confines the pointer (many games) gets the mouse to
  itself: the emulator then captures the mouse on a click, as usual.
- Clipboard: text only, up to 512K characters. Windows 9x/ME keeps text
  in its ANSI code page, so characters outside it arrive as "?".
