/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The PCMCIA settings: whether the PC Card controller
 *             (pcic_pd6722.c) is fitted, and the card in each of its two
 *             sockets, with a network card's link.
 *
 *             In 86box.cfg:
 *
 *               [PCMCIA]
 *               controller = 1
 *               socket_a = te100pc16          card internal names
 *               socket_a_net_type = slirp     none, slirp or pcap
 *               socket_a_net_host = ...       the pcap host interface
 *
 *             and each card's own settings in its device section, one
 *             instance per socket ("... #1" for A, "#2" for B).
 *
 *             A network card links through the NET_CARD_MAX + socket entry of
 *             net_cards_conf, after the four of the Network settings, so it
 *             takes none of those.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/thread.h>
#include <86box/timer.h>
#include <86box/network.h>
#include <86box/pcmcia.h>

extern const device_t te100pc16_device;

int  pcmcia_enabled;
int  pcmcia_card_type[PCMCIA_SOCKETS];
int  pcmcia_net_type[PCMCIA_SOCKETS];
char pcmcia_net_host[PCMCIA_SOCKETS][128];

static const struct {
    const device_t *dev;
    int             network;
} cards[] = {
    // clang-format off
    { &device_none,       0 },
    { &te100pc16_device,  1 },
    { NULL,               0 }
    // clang-format on
};

void
pcmcia_reset(void)
{
    if (!pcmcia_enabled)
        return;

    pcmcia_controller_add();
    for (int s = 0; s < PCMCIA_SOCKETS; s++) {
        const int c = pcmcia_card_type[s];
        if ((c > 0) && (c < pcmcia_card_count()))
            device_add_inst(cards[c].dev, s + 1);
    }
}

int
pcmcia_card_count(void)
{
    int n = 0;

    while (cards[n].dev)
        n++;
    return n;
}

const char *
pcmcia_card_get_internal_name(int card)
{
    return device_get_internal_name(cards[card].dev);
}

const char *
pcmcia_card_get_name(int card)
{
    return cards[card].dev->name;
}

int
pcmcia_card_get_from_internal_name(const char *s)
{
    for (int c = 0; cards[c].dev; c++)
        if (!strcmp(cards[c].dev->internal_name, s))
            return c;
    return 0;
}

int
pcmcia_card_has_config(int card)
{
    return (card > 0) && (card < pcmcia_card_count()) && device_has_config(cards[card].dev);
}

int
pcmcia_card_is_network(int card)
{
    return (card > 0) && (card < pcmcia_card_count()) && cards[card].network;
}

const device_t *
pcmcia_card_get_device(int card)
{
    return cards[card].dev;
}

netcard_t *
pcmcia_network_attach(int socket, void *card_drv, uint8_t *mac, NETRXCB rx)
{
    const int       slot  = NET_CARD_MAX + socket;
    netcard_conf_t *conf  = &net_cards_conf[slot];
    const uint16_t  saved = net_card_current;
    netcard_t      *card;

    memset(conf, 0, sizeof(netcard_conf_t));
    conf->net_type = pcmcia_net_type[socket];
    snprintf(conf->host_dev_name, sizeof(conf->host_dev_name), "%s", pcmcia_net_host[socket]);

    net_card_current = slot;
    card             = network_attach(card_drv, mac, rx, NULL);
    net_card_current = saved;

    /* A link that failed to come up was turned into none: so be it in the
       settings too, as for the adapters of the Network settings. */
    pcmcia_net_type[socket] = conf->net_type;
    return card;
}
