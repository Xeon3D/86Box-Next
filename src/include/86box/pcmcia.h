/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             PC Card (PCMCIA) support: the socket controller (pcic_pd6722.c),
 *             the cards that go in its sockets, their Card Information
 *             Structures (pccard_cis.c), and the PCMCIA settings (pcmcia.c).
 *             The controller and the TRENDnet TE100-PC16 card come from
 *             MegaPPBox, where they are the PC Card slots of the Merit MAXX
 *             I/O board.
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
   resets a card too.  A card without common memory leaves common_read and
   common_write NULL; one without I/O, the io_ ones. */
typedef struct pccard_t {
    const char *name;   /* for the PC Card menu */
    uint8_t (*attr_read)(uint32_t addr, void *priv);
    void (*attr_write)(uint32_t addr, uint8_t val, void *priv);
    uint8_t (*common_read)(uint32_t addr, void *priv);
    void (*common_write)(uint32_t addr, uint8_t val, void *priv);
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
extern int  pcmcia_controller_present(void);

/* A card device plugs itself into its socket (0 = A, 1 = B) when it is
   created, and takes itself out (NULL) when it is closed.  A card may
   arrive before the controller does; it is picked up when the controller
   is created. */
extern void pcmcia_insert(int socket, const pccard_t *card);

/* The user taking a card out of its socket and putting it back (the PC Card
   menu): the card stays fitted, the socket sees it go and come, with the
   card-detect change the guest's socket services wait for.  Safe from the UI
   thread; applied on the emulation thread. */
extern void        pcmcia_eject(int socket, int ejected);
extern int         pcmcia_ejected(int socket);
extern const char *pcmcia_socket_card_name(int socket);   /* NULL: no card fitted */

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

/* ------------------------------------------- Card Information Structure --- */

#define CISTPL_NULL          0x00
#define CISTPL_DEVICE        0x01
#define CISTPL_NO_LINK       0x14
#define CISTPL_VERS_1        0x15
#define CISTPL_DEVICE_A      0x17
#define CISTPL_CONFIG        0x1a
#define CISTPL_CFTABLE_ENTRY 0x1b
#define CISTPL_MANFID        0x20
#define CISTPL_FUNCID        0x21
#define CISTPL_FUNCE         0x22
#define CISTPL_END           0xff

#define CISTPL_FUNCID_MEMORY  0x01
#define CISTPL_FUNCID_NETWORK 0x06

typedef struct pccard_cis_t {
    uint8_t *buf;
    int      len, max;
    int      filler;   /* where the CONFIG tuple's two filler bytes are */
} pccard_cis_t;

extern void     pccard_cis_init(pccard_cis_t *c, uint8_t *buf, int max);
extern void     pccard_cis_tuple(pccard_cis_t *c, uint8_t code, const uint8_t *data, int len);
extern void     pccard_cis_vers1(pccard_cis_t *c, const char *const *s, int n);
extern void     pccard_cis_manfid(pccard_cis_t *c, uint16_t manf, uint16_t card);
extern void     pccard_cis_config(pccard_cis_t *c, uint16_t base, uint8_t rmask, uint8_t last);
extern void     pccard_cis_end(pccard_cis_t *c);
extern uint16_t pccard_cis_win9x_crc(const uint8_t *cis, int len);
extern void     pccard_cis_win9x_id(const uint8_t *cis, int len, char *out, int outlen);
extern int      pccard_cis_match_id(pccard_cis_t *c, uint16_t crc);

/* Each card's CIS, as its device builds it (and the CIS tests check it):
   into buf (at least 256 bytes), returning its length. */
extern int pccard_cis_te100pc16(uint8_t *buf, const uint8_t mac[6]);
extern int pccard_cis_3c589d(uint8_t *buf);
extern int pccard_cis_sram(uint8_t *buf, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /*EMU_PCMCIA_H*/
