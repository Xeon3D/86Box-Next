/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             Card Information Structures for the emulated PC Cards: a
 *             builder, the rules a CIS must follow to be read the way the
 *             guests read it, and the Plug and Play ID Windows 9x makes of
 *             one.
 *
 *             How the guests walk a CIS (Linux pcmcia-cs modules/cistpl.c,
 *             and Windows 98's PCCARD.VXD the same way): tuples from
 *             attribute memory 0, NULL tuples skipped, to CISTPL_END; with
 *             no CISTPL_NO_LINK they then assume a long link to common
 *             memory 0 and look there for a CISTPL_LINKTARGET.  A card
 *             without common memory therefore carries NO_LINK.
 *
 *             The Windows 9x ID of a single-function card is
 *             PCMCIA\<string 1>-<string 2>-<CRC>: the first two VERS_1
 *             strings, printable characters only, spaces made underscores,
 *             and a 16-bit checksum PCCARD.VXD takes through CONFIGMG's
 *             CM_Get_CRC_CheckSum over the bodies of the DEVICE, VERS_1
 *             (through its second string), CONFIG, CFTABLE_ENTRY and MANFID
 *             tuples in CIS order, from a seed of 0.  The checksum is
 *             CRC-16/ARC computed a nibble at a time from two 16-entry
 *             tables -- except that the high-nibble table's last entry is
 *             0x4600 where ARC has 0x4400, so it is not quite ARC.  (Taken
 *             from the two VxDs and checked by running them under an x86
 *             emulator against IDs Windows 98 gave three CIS variants.)
 *
 *             An INF lists the IDs of the real cards, checksum included;
 *             an emulated CIS is not the real one byte for byte, so its
 *             CONFIG tuple ends in a two-byte vendor subtuple (code 0xC1,
 *             which every parser skips) chosen to make the checksum the
 *             one the INF names (pccard_cis_match_id()).
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/pcmcia.h>

/* ---------------------------------------------------------- building --- */

void
pccard_cis_init(pccard_cis_t *c, uint8_t *buf, int max)
{
    c->buf    = buf;
    c->len    = 0;
    c->max    = max;
    c->filler = -1;
    memset(buf, 0xff, max);
}

static void
put(pccard_cis_t *c, uint8_t b)
{
    if (c->len < c->max)
        c->buf[c->len] = b;
    c->len++;
}

void
pccard_cis_tuple(pccard_cis_t *c, uint8_t code, const uint8_t *data, int len)
{
    put(c, code);
    put(c, (uint8_t) len);
    for (int i = 0; i < len; i++)
        put(c, data[i]);
}

/* VERS_1, PC Card 2.1 and later (4.1): the strings, then 0xFF. */
void
pccard_cis_vers1(pccard_cis_t *c, const char *const *s, int n)
{
    int at;

    put(c, CISTPL_VERS_1);
    at = c->len;
    put(c, 0);
    put(c, 0x04);
    put(c, 0x01);
    for (int i = 0; i < n; i++) {
        for (const char *p = s[i]; *p; p++)
            put(c, (uint8_t) *p);
        put(c, 0x00);
    }
    put(c, 0xff);
    if (at < c->max)
        c->buf[at] = (uint8_t) (c->len - at - 1);
}

void
pccard_cis_manfid(pccard_cis_t *c, uint16_t manf, uint16_t card)
{
    const uint8_t d[4] = { manf & 0xff, manf >> 8, card & 0xff, card >> 8 };
    pccard_cis_tuple(c, CISTPL_MANFID, d, 4);
}

/* CONFIG: registers at base (a two-byte address), present mask rmask, last
   configuration index last, and the two-byte filler subtuple, set later by
   pccard_cis_match_id(). */
void
pccard_cis_config(pccard_cis_t *c, uint16_t base, uint8_t rmask, uint8_t last)
{
    const uint8_t d[9] = { 0x01, last, base & 0xff, base >> 8, rmask, 0xc1, 0x02, 0x00, 0x00 };
    pccard_cis_tuple(c, CISTPL_CONFIG, d, 9);
    c->filler = c->len - 2;
}

void
pccard_cis_end(pccard_cis_t *c)
{
    put(c, CISTPL_END);
}

/* -------------------------------------------------------- the Win9x ID --- */

static const uint16_t crc_lo[16] = {
    0x0000, 0xc0c1, 0xc181, 0x0140, 0xc301, 0x03c0, 0x0280, 0xc241,
    0xc601, 0x06c0, 0x0780, 0xc741, 0x0500, 0xc5c1, 0xc481, 0x0440
};
static const uint16_t crc_hi[16] = {
    0x0000, 0xcc01, 0xd801, 0x1400, 0xf001, 0x3c00, 0x2800, 0xe401,
    0xa001, 0x6c00, 0x7800, 0xb401, 0x5000, 0x9c01, 0x8801, 0x4600   /* sic: 0x4400 in ARC */
};

static uint16_t
win9x_crc(uint16_t c, const uint8_t *p, int n)
{
    while (n--) {
        const uint8_t x = (uint8_t) (c ^ *p++);
        c = (uint16_t) ((c >> 8) ^ crc_lo[x & 15] ^ crc_hi[x >> 4]);
    }
    return c;
}

uint16_t
pccard_cis_win9x_crc(const uint8_t *cis, int len)
{
    uint32_t cfg_base = 0;
    uint16_t c        = 0;

    /* The configuration registers' address, first: a tuple running over
       them is cut short where they start. */
    for (int i = 0; (i + 1 < len) && (cis[i] != CISTPL_END);) {
        if (cis[i] == CISTPL_NULL) {
            i++;
            continue;
        }
        if ((cis[i] == CISTPL_CONFIG) && (cis[i + 1] >= 4)) {
            const int rasz = cis[i + 2] & 3;
            for (int k = 0; k <= rasz; k++)
                cfg_base |= (uint32_t) cis[i + 4 + k] << (8 * k);
            break;
        }
        i += 2 + cis[i + 1];
    }

    for (int i = 0; (i + 1 < len) && (cis[i] != CISTPL_END);) {
        const uint8_t code = cis[i];
        int           n;

        if (code == CISTPL_NULL) {
            i++;
            continue;
        }
        n = cis[i + 1];
        if ((code == CISTPL_DEVICE) || (code == CISTPL_VERS_1) || (code == CISTPL_CONFIG) || (code == CISTPL_CFTABLE_ENTRY)
            || (code == CISTPL_MANFID)) {
            const uint8_t *d   = &cis[i + 2];
            const uint32_t off = (uint32_t) i * 2;   /* attribute memory address */
            int            m   = n;

            if ((cfg_base >= off + 4) && (cfg_base < off + 4 + 2 * (uint32_t) n))
                m = (int) (cfg_base - off - 4) >> 1;
            if (code == CISTPL_VERS_1) {
                int j = 2;
                while ((j < n) && d[j])
                    j++;
                j++;
                while ((j < n) && d[j])
                    j++;
                j++;
                if (m > j)
                    m = j;
            }
            c = win9x_crc(c, d, m);
        }
        i += 2 + n;
    }
    return c;
}

/* PCMCIA\<string 1>-<string 2>-<CRC>, as PCCARD.VXD makes it. */
void
pccard_cis_win9x_id(const uint8_t *cis, int len, char *out, int outlen)
{
    char ids[2][64] = { "", "" };

    for (int i = 0; (i + 1 < len) && (cis[i] != CISTPL_END);) {
        if (cis[i] == CISTPL_NULL) {
            i++;
            continue;
        }
        if (cis[i] != CISTPL_VERS_1) {
            i += 2 + cis[i + 1];
            continue;
        }
        const uint8_t *d = &cis[i + 4];
        const uint8_t *e = &cis[i + 2 + cis[i + 1]];
        for (int s = 0; (s < 2) && (d < e) && (*d != 0xff); s++) {
            int k = 0;
            for (; (d < e) && *d && (*d != 0xff); d++)
                if ((*d >= 0x20) && (*d < 0x7f) && (k < 63))
                    ids[s][k++] = (*d == ' ') ? '_' : (char) *d;
            ids[s][k] = '\0';
            if ((d < e) && (*d == 0))
                d++;
        }
        break;
    }
    snprintf(out, outlen, "PCMCIA\\%s-%s-%04X", ids[0], ids[1], pccard_cis_win9x_crc(cis, len));
}

/* Set the CONFIG tuple's filler bytes so Windows 9x gives the card the ID
   ending in crc.  Returns 1 when it does. */
int
pccard_cis_match_id(pccard_cis_t *c, uint16_t crc)
{
    if (c->filler < 0)
        return 0;
    for (int v = 0; v < 0x10000; v++) {
        c->buf[c->filler]     = (uint8_t) v;
        c->buf[c->filler + 1] = (uint8_t) (v >> 8);
        if (pccard_cis_win9x_crc(c->buf, c->len) == crc)
            return 1;
    }
    c->buf[c->filler] = c->buf[c->filler + 1] = 0;
    return 0;
}

/* ------------------------------------------------------- the cards' CIS --- */

/* An I/O card's one configuration: index 1 (the default), I/O interface,
   5 V, len ports at base on lines address lines, 8 and 16 bit, any of the
   IRQs in irqs, level-triggered. */
static void
cftable_io(pccard_cis_t *c, uint16_t base, uint8_t len, uint8_t lines, uint16_t irqs)
{
    const uint8_t d[13] = {
        0xc1, 0x01,                     /* index 1, default; I/O interface    */
        0x19,                           /* features: Vcc, I/O, IRQ            */
        0x01, 0x55,                     /* Vcc: nominal 5 V                   */
        (uint8_t) (0xe0 | lines),       /* range follows, 16 and 8 bit, lines */
        0x60, base & 0xff, base >> 8,   /* one range, 2-byte base, 1-byte len */
        (uint8_t) (len - 1),
        0x30, irqs & 0xff, irqs >> 8    /* IRQ mask follows, level            */
    };
    pccard_cis_tuple(c, CISTPL_CFTABLE_ENTRY, d, 13);
}

static const uint8_t no_device[3] = { 0x00, 0x00, 0xff };   /* DEVICE: no common memory */
static const uint8_t fn_network[2] = { CISTPL_FUNCID_NETWORK, 0x00 };

/* TRENDnet TE100-PC16 (net_te100pc16.c).  Its driver's INF knows it as
   PCMCIA\Fast_Ethernet-16-bit_PC_Card-8B43. */
int
pccard_cis_te100pc16(uint8_t *buf, const uint8_t mac[6])
{
    static const char *vers[] = { "Fast Ethernet", "16-bit PC Card", "", "AX88190" };
    pccard_cis_t       c;
    uint8_t            node[8] = { 0x04, 0x06 };

    memcpy(node + 2, mac, 6);
    pccard_cis_init(&c, buf, 256);
    pccard_cis_tuple(&c, CISTPL_DEVICE, no_device, 3);
    pccard_cis_vers1(&c, vers, 4);
    pccard_cis_manfid(&c, 0x0149, 0xc1ab);
    pccard_cis_tuple(&c, CISTPL_FUNCID, fn_network, 2);
    pccard_cis_tuple(&c, CISTPL_FUNCE, node, 8);                /* LAN node ID */
    pccard_cis_config(&c, 0x03c0, 0x03, 0x01);                  /* COR, CCSR */
    cftable_io(&c, 0x0300, 32, 5, 0xdeb8);
    pccard_cis_tuple(&c, CISTPL_NO_LINK, NULL, 0);
    pccard_cis_end(&c);
    pccard_cis_match_id(&c, 0x8b43);
    return c.len;
}

/* 3Com 3C589D EtherLink III LAN PC Card (net_3c509b.c's PC Card model):
   sixteen ports decoding all sixteen address lines, the station address in
   its EEPROM.  Windows 98's own NET3C589.INF knows it as
   PCMCIA\3Com_Corporation-3C589D-9CA6. */
int
pccard_cis_3c589d(uint8_t *buf)
{
    static const char *vers[] = { "3Com Corporation", "3C589D", "TP/BNC LAN Card Ver. 2a", "000002" };
    pccard_cis_t       c;

    pccard_cis_init(&c, buf, 256);
    pccard_cis_tuple(&c, CISTPL_DEVICE, no_device, 3);
    pccard_cis_vers1(&c, vers, 4);
    pccard_cis_manfid(&c, 0x0101, 0x0589);
    pccard_cis_tuple(&c, CISTPL_FUNCID, fn_network, 2);
    pccard_cis_config(&c, 0x0200, 0x01, 0x01);                  /* COR */
    cftable_io(&c, 0x0300, 16, 16, 0xdeb8);
    pccard_cis_tuple(&c, CISTPL_NO_LINK, NULL, 0);
    pccard_cis_end(&c);
    pccard_cis_match_id(&c, 0x9ca6);
    return c.len;
}

/* An SRAM memory card of size bytes (64 KB to 4 MB): common memory
   described by the DEVICE tuple -- type SRAM, 150 ns, its size as units
   times a power of four of 512 bytes -- and a memory function; no
   configuration registers, which a memory card does not have. */
int
pccard_cis_sram(uint8_t *buf, uint32_t size)
{
    static const char *vers[] = { "86Box-Next", "SRAM Card", "" };
    pccard_cis_t       c;
    uint8_t            dev[3] = { 0x63, 0x00, 0xff };   /* SRAM, 150 ns */
    const uint8_t      fn_memory[2] = { CISTPL_FUNCID_MEMORY, 0x00 };

    for (int scale = 0; scale < 7; scale++) {
        const uint32_t unit = 512u << (scale * 2);
        if (!(size % unit) && (size / unit >= 1) && (size / unit <= 32)) {
            dev[1] = (uint8_t) (((size / unit - 1) << 3) | scale);
            break;
        }
    }
    pccard_cis_init(&c, buf, 256);
    pccard_cis_tuple(&c, CISTPL_DEVICE, dev, 3);
    pccard_cis_vers1(&c, vers, 3);
    pccard_cis_tuple(&c, CISTPL_FUNCID, fn_memory, 2);
    pccard_cis_tuple(&c, CISTPL_NO_LINK, NULL, 0);
    pccard_cis_end(&c);
    return c.len;
}
