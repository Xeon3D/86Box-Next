/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Arcade cabinet I/O boards: the ISA cards that carry a cabinet's
 *             coin mechanisms and operator buttons.  A machine has at most
 *             one -- they are a single selection in Settings, not slots --
 *             and the fitted one decides which cabinet controls the UI shows.
 *
 *             The boards themselves come from the single-cabinet forks:
 *               funworld_io.c  PeepeeBox  (funworld Photo Play / I.G.O.)
 *               merit_io.c     MegaPPBox  (Merit Megatouch XL and MAXX)
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/funworld_io.h>
#include <86box/merit_io.h>
#include <86box/io_board.h>

int io_board_type = IO_BOARD_NONE;

static const struct {
    const device_t *dev;
} boards[] = {
    // clang-format off
    [IO_BOARD_NONE]       = { &device_none          },
    [IO_BOARD_FUNWORLD]   = { &funworld_io_device   },
    [IO_BOARD_MERIT_XL]   = { &merit_io_xl_device   },
    [IO_BOARD_MERIT_MAXX] = { &merit_io_maxx_device },
    { NULL }
    // clang-format on
};

void
io_board_reset(void)
{
    if ((io_board_type <= IO_BOARD_NONE) || (io_board_type >= IO_BOARD_COUNT))
        return;

    device_add(boards[io_board_type].dev);
}

const char *
io_board_get_internal_name(int board)
{
    return device_get_internal_name(boards[board].dev);
}

int
io_board_get_from_internal_name(const char *str)
{
    for (int c = 0; boards[c].dev != NULL; c++) {
        if (!strcmp(boards[c].dev->internal_name, str))
            return c;
    }

    /* Not found. */
    return IO_BOARD_NONE;
}

const device_t *
io_board_get_device(int board)
{
    return boards[board].dev;
}

int
io_board_has_config(int board)
{
    if (boards[board].dev == NULL)
        return 0;

    return (boards[board].dev->config ? 1 : 0);
}
