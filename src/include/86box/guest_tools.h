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

/* debug_cmd's "gt" command. */
extern void guest_tools_debug(const char *args);

#ifdef __cplusplus
}
#endif

#endif /* EMU_GUEST_TOOLS_H */
