/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          86Box Guest Tools: the emulator side (pointer integration and
 *          clipboard sharing with an agent in the guest).
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef EMU_GUEST_TOOLS_H
#define EMU_GUEST_TOOLS_H

#include <stddef.h>
#include <stdint.h>
#include <86box/guest_tools_proto.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set while the device is in: lets guest code of any privilege reach its ports. */
extern int guest_tools_ports_open;

#define guest_tools_io_bypass(port) \
    (guest_tools_ports_open && (((port) & ~(GT_PORT_COUNT - 1)) == GT_PORT_BASE))

#ifdef EMU_DEVICE_H
extern const device_t guest_tools_device;
#endif

/* Pointer integration is live: the agent follows the host pointer, so the
   UI must not capture the mouse and sends only buttons and the wheel. */
extern int  guest_tools_pointer_active(void);

/* The host pointer is over the integrated guest's display: the keyboard is
   the guest's, as if the mouse were captured ("Keyboard requires capture"). */
extern int  guest_tools_keyboard(void);

/* Integrated, and the host pointer is off the guest's display: the keys
   are the host's, not the machine's. */
extern int  guest_tools_keyboard_outside(void);

/* The host pointer over the guest display (UI thread): x and y from 0.0 to
   1.0 across it; inside = 0 when the pointer left it. */
extern void guest_tools_host_pointer(double x, double y, int inside);

/* The host clipboard changed (UI thread). */
extern void guest_tools_host_clipboard_changed(void);

/* The UI's clipboard access, both called on the emulation thread with text
   as UTF-16, LF line ends, no terminator: set_host must copy the text and
   hand it to its own thread; get_host returns a malloc()ed copy of the host
   clipboard text (NULL when it holds none), which the caller frees. */
extern void guest_tools_set_clipboard_handlers(void (*set_host)(const uint16_t *text, size_t chars),
                                               uint16_t *(*get_host)(size_t *chars));

/* The UI's side of copying files, all called on the emulation thread.
   Host to guest: list() takes a snapshot of the files on the host clipboard
   (source GT_SOURCE_CLIPBOARD) or of the host drag over the display
   (GT_SOURCE_DRAG) and returns how many entries it has (-1: none); entry()
   and read() work on that source's snapshot (read returns the bytes read,
   -1 on failure). Guest to host: begin() starts an empty set (0 on
   success), add() makes a directory or an empty file in it (its index, -1
   on failure; name is checked by the UI), write() writes into an added file
   (0 on success), end() puts the set on the host clipboard or drops it
   (GT_FILES_*). Names are UTF-16, '\' between their parts. */
typedef struct guest_tools_file_ops_t {
    int (*list)(int source);
    int (*entry)(int source, int index, uint32_t *flags, uint64_t *size, const uint16_t **name, size_t *chars);
    int (*read)(int source, int index, uint64_t offset, void *buf, uint32_t bytes);
    int (*begin)(void);
    int (*add)(uint32_t flags, uint64_t size, const uint16_t *name, size_t chars);
    int (*write)(int index, uint64_t offset, const void *buf, uint32_t bytes);
    int (*end)(int action);
    /* debug_cmd "gt files": a stand-in host clipboard holding these files
       (UTF-8, '|' between them); from then on the guest's files are logged,
       not put on the host clipboard. */
    void (*debug)(const char *paths);
} guest_tools_file_ops_t;

#define GT_SOURCE_CLIPBOARD 0
#define GT_SOURCE_DRAG      1

extern void guest_tools_set_file_ops(const guest_tools_file_ops_t *ops);

/* Files go both ways with the clipboard: a host drag of files may drop on
   the guest (UI thread). */
extern int  guest_tools_files_enabled(void);

/* The host drags files over the guest display (UI thread): 1 entering or
   moving, 2 dropped, 0 gone. */
extern void guest_tools_host_drag(int state);

/* The agent caught a guest drag of files leaving the display: the UI lets
   go of the guest's button and carries the drag on (UI thread). */
extern int  guest_tools_drag_out(void);

/* The running agent's version (GT_TOOLS_VERSION form), 1 for an agent too
   old to say, 0 when no agent runs. */
extern unsigned int guest_tools_agent_version(void);

/* The UI's Tools > Install 86Box Guest Tools, for debug_cmd's "gt insert"
   (called on the emulation thread; the UI runs it on its own). */
extern void (*guest_tools_insert_media_hook)(void);

/* debug_cmd's "gt" command. */
extern void guest_tools_debug(const char *args);

#ifdef __cplusplus
}
#endif

#endif /* EMU_GUEST_TOOLS_H */
