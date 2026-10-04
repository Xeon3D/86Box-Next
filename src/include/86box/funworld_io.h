/*
 * PeepeeBox   A fork of 86Box that emulates the funworld Photo Play / I.G.O.
 *             arcade kiosk hardware, including its protection token.
 *
 *             The funworld I/O card -- the 8255 the coin acceptor and the two
 *             service buttons hang off.  See funworld_io.c.
 *
 * Authors:    The HUEG PP team.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_FUNWORLD_IO_H
#define EMU_FUNWORLD_IO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The lines that arrive at the card.  Six coins because the Coin Controls C120
   has six separate accept outputs, one per programmed coin, rather than
   encoding the value in a pulse count -- and four notes, because the machine
   takes 5, 10, 20 and 50 EUR paper from a second validator on the same loom.

   Which denomination each one carries is no longer a guess; see the table in
   funworld_io.c.  The names here are the money, not the pin, because the pin is
   the part that changed twice. */
#define FWIO_LINE_COIN1 0          /* 0.10 EUR   */
#define FWIO_LINE_COIN2 1          /* 0.20 EUR   */
#define FWIO_LINE_COIN3 2          /* 0.50 EUR   */
#define FWIO_LINE_COIN4 3          /* 1.00 EUR   */
#define FWIO_LINE_COIN5 4          /* 2.00 EUR   */
#define FWIO_LINE_COIN6 5          /* TOKEN 10   */
#define FWIO_LINE_SETUP 6          /* A0, confirmed                          */
#define FWIO_LINE_DOOR2 7          /* A1, the other door button.  From the
                                      menu the software answers it with a CRC
                                      check; it was called the calibration
                                      button on the strength of the cabinet
                                      having two buttons, which is not
                                      evidence.  Where the touchscreen
                                      calibration lives is still open.        */
#define FWIO_LINE_NOTE1 8          /* 5 EUR      */
#define FWIO_LINE_NOTE2 9          /* 10 EUR     */
#define FWIO_LINE_NOTE3 10         /* 20 EUR     */
#define FWIO_LINE_NOTE4 11         /* 50 EUR     */
#define FWIO_IN_LINES   12

/* Assert one line for as long as the real part would hold it -- 100 ms for a
   C120 coin, which is what the software debounces against.  Called from the UI;
   does nothing when the card is not fitted. */
extern void funworld_io_pulse(int line);

/* Whether the card is in the machine, so the UI can grey its buttons. */
extern int  funworld_io_present(void);

/* Diagnostic: answer the card-detection sweep at every candidate address, to
   find out which one the disk expects.  Enabled by PEEPEEBOX_IO_PROBE. */
extern void funworld_io_probe_init(void);

/* Diagnostic: whether PEEPEEBOX_IO_WALK is on, and which line the last pulse
   held.  Returns 0 when the walk is not running. */
extern int  funworld_io_walk_state(char *out, size_t len);

/* There was a Coin Controls C120 device here that attached to COM2, on the
   reading that the validator was on the serial port.  It was not: all ten money
   lines are on the I/O card, measured one at a time on I.G.O. 6.  It is gone,
   and COM2 is free again -- which matters, because these cabinets could carry a
   receipt printer there instead of the I.G.O. 8 serial dongle. */
extern const device_t funworld_io_device;

#ifdef __cplusplus
}
#endif

#endif /*EMU_FUNWORLD_IO_H*/
