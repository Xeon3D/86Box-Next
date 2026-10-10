/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Internal (ISA) modem cards: one per machine, Settings > Other
 *             peripherals.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_MODEM_CARD_H
#define EMU_MODEM_CARD_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MODEM_CARD_NONE = 0,
    MODEM_CARD_SUPRA56I, /* Diamond SupraExpress 56i Sp, ISA PnP SUP2171 */
    MODEM_CARD_COUNT
};

extern int modem_card_type; /* (C) the fitted internal modem */

extern void        modem_card_reset(void);
extern const char *modem_card_get_internal_name(int card);
extern int         modem_card_get_from_internal_name(const char *str);
extern int         modem_card_has_config(int card);
#ifdef EMU_DEVICE_H
extern const device_t *modem_card_get_device(int card);

extern const device_t supra56i_device;
#endif

#ifdef __cplusplus
}
#endif

#endif /*EMU_MODEM_CARD_H*/
