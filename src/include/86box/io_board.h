/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Arcade cabinet I/O boards.  See io_board.c.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_IO_BOARD_H
#define EMU_IO_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    IO_BOARD_NONE = 0,
    IO_BOARD_FUNWORLD,   /* funworld Photo Play I/O card (PeepeeBox)       */
    IO_BOARD_MERIT_XL,   /* Merit Megatouch XL, CRT-500 Zeus (MegaPPBox)    */
    IO_BOARD_MERIT_MAXX, /* Merit Megatouch MAXX, Millennium (MegaPPBox)   */
    IO_BOARD_COUNT
};

extern int io_board_type; /* (C) the fitted arcade I/O board, one per machine */

extern void        io_board_reset(void);
extern const char *io_board_get_internal_name(int board);
extern int         io_board_get_from_internal_name(const char *str);
extern int         io_board_has_config(int board);
#ifdef EMU_DEVICE_H
extern const device_t *io_board_get_device(int board);
#endif

#ifdef __cplusplus
}
#endif

#endif /*EMU_IO_BOARD_H*/
