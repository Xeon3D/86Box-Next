/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             PC Card (PCMCIA) support: the socket controller (pcic_pd6722.c),
 *             the cards that go in its sockets, and the PCMCIA settings
 *             (pcmcia.c).  The controller and the TRENDnet TE100-PC16 card
 *             come from MegaPPBox, where they are the PC Card slots of the
 *             Merit MAXX I/O board.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#ifndef EMU_PCMCIA_H
#define EMU_PCMCIA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCMCIA_SOCKETS 2

/* A 16-bit PC Card as the socket sees it.  Addresses are card addresses:
   attribute and common memory from 0, I/O as the host port (a card decodes
   only its own low address lines).  reset is the RESET pin, and power-off
   resets a card too. */
typedef struct pccard_t {
    uint8_t (*attr_read)(uint32_t addr, void *priv);
    void (*attr_write)(uint32_t addr, uint8_t val, void *priv);
    uint8_t (*io_read)(uint16_t port, void *priv);
    uint16_t (*io_readw)(uint16_t port, void *priv);
    void (*io_write)(uint16_t port, uint8_t val, void *priv);
    void (*io_writew)(uint16_t port, uint16_t val, void *priv);
    void (*reset)(void *priv);
    void *priv;
} pccard_t;

/* The controller: added once, by the PCMCIA settings or by an I/O board that
   carries PC Card slots (the Merit MAXX), whichever comes first. */
extern void pcmcia_controller_add(void);

/* Plug a card into a socket (0 = A, 1 = B), or pull it out (NULL).  A card
   may arrive before the controller does; it is picked up when the
   controller is created. */
extern void pcmcia_insert(int socket, const pccard_t *card);

/* The card's IREQ line: 1 = interrupt requested.  The controller steers it
   to the ISA IRQ the driver chose, once the card is in I/O mode. */
extern void pcmcia_card_irq(int socket, int level);

/* The PCMCIA settings (86box.cfg, [PCMCIA]). */
extern int  pcmcia_enabled;                      /* the controller is fitted   */
extern int  pcmcia_card_type[PCMCIA_SOCKETS];    /* the card in each socket    */
extern int  pcmcia_net_type[PCMCIA_SOCKETS];     /* a network card's link      */
extern char pcmcia_net_host[PCMCIA_SOCKETS][128]; /* and its host interface    */

extern void        pcmcia_reset(void);
extern int         pcmcia_card_count(void);
extern const char *pcmcia_card_get_internal_name(int card);
extern const char *pcmcia_card_get_name(int card);
extern int         pcmcia_card_get_from_internal_name(const char *s);
extern int         pcmcia_card_has_config(int card);
extern int         pcmcia_card_is_network(int card);
#ifdef EMU_DEVICE_H
extern const device_t *pcmcia_card_get_device(int card);
#endif

/* For a network card: attach to the network the way network_attach() does,
   with the link chosen for its socket in the PCMCIA settings. */
#ifdef EMU_NETWORK_H
extern netcard_t *pcmcia_network_attach(int socket, void *card_drv, uint8_t *mac, NETRXCB rx);
#endif

#ifdef __cplusplus
}
#endif

#endif /*EMU_PCMCIA_H*/
