/*
 * 86Box-Next  A fork of 86Box with extra features.
 *
 *             The physical USB ports: what is plugged into each, the plug and
 *             unplug requests from the UI (applied on the emulation thread at
 *             the next frame), the ports remembered in the config, and which
 *             controller the ports are wired to -- the standalone UHCI card,
 *             or the EHCI card, which hands each port to itself or to its
 *             companion UHCI as the guest's driver decides.
 *
 *             Released under the GNU General Public License version 2 or
 *             later.  See COPYING for more information.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/thread.h>
#include <86box/usb_next.h>

char usbn_port_cfg[USBN_PORTS][16];

static mutex_t              *lock;
static usbn_device_t        *devs[USBN_PORTS];    /* plugged in, emulation side   */
static usbn_device_t        *plug[USBN_PORTS];    /* waiting to be plugged in     */
static int                   unplug[USBN_PORTS];  /* waiting to be pulled out     */
static const usbn_root_ops_t *root;
static void                 *root_priv;
static int                   root_high_speed;

static void
bus_lock(void)
{
    if (lock == NULL)
        lock = thread_create_mutex();
    thread_wait_mutex(lock);
}

static void
bus_unlock(void)
{
    thread_release_mutex(lock);
}

void
usbn_set_root(const usbn_root_ops_t *ops, void *priv, int high_speed)
{
    bus_lock();
    if (ops == NULL) {
        /* The card is going away: everything plugged in is released. */
        for (int p = 0; p < USBN_PORTS; p++) {
            if (devs[p] && devs[p]->destroy)
                devs[p]->destroy(devs[p]);
            if (plug[p] && plug[p]->destroy)
                plug[p]->destroy(plug[p]);
            devs[p] = plug[p] = NULL;
            unplug[p]         = 0;
        }
    }
    root            = ops;
    root_priv       = priv;
    root_high_speed = high_speed;
    bus_unlock();
}

int
usbn_present(void)
{
    return root != NULL;
}

int
usbn_bus_high_speed(void)
{
    return root_high_speed;
}

usbn_device_t *
usbn_port_device(int port)
{
    return ((port >= 0) && (port < USBN_PORTS)) ? devs[port] : NULL;
}

/* Called by the root controller once per frame, on the emulation thread. */
void
usbn_apply(void)
{
    if (root == NULL)
        return;
    bus_lock();
    for (int p = 0; p < USBN_PORTS; p++) {
        if (unplug[p]) {
            unplug[p] = 0;
            if (devs[p]) {
                usbn_device_t *d = devs[p];
                root->disconnect(root_priv, p);
                devs[p] = NULL;
                pclog("USB: %s unplugged from port %d\n", d->name, p + 1);
                if (d->destroy)
                    d->destroy(d);
            }
        }
        if (plug[p] && (devs[p] == NULL)) {
            devs[p] = plug[p];
            plug[p] = NULL;
            pclog("USB: %s plugged into port %d\n", devs[p]->name, p + 1);
            root->connect(root_priv, p, devs[p]);
        }
    }
    bus_unlock();
}

int
usbn_port_busy(int port)
{
    int busy;

    if (!root || (port < 0) || (port >= USBN_PORTS))
        return 1;
    bus_lock();
    busy = (devs[port] != NULL && !unplug[port]) || (plug[port] != NULL);
    bus_unlock();
    return busy;
}

const char *
usbn_port_name(int port, char *buf, int len)
{
    buf[0] = '\0';
    if ((port < 0) || (port >= USBN_PORTS))
        return buf;
    bus_lock();
    if (plug[port])
        snprintf(buf, len, "%s", plug[port]->name);
    else if (devs[port] && !unplug[port])
        snprintf(buf, len, "%s", devs[port]->name);
    bus_unlock();
    return buf;
}

int
usbn_attach(int port, usbn_device_t *d)
{
    if (!root || !d || (port < 0) || (port >= USBN_PORTS) || usbn_port_busy(port))
        return 0;
    bus_lock();
    plug[port] = d;
    bus_unlock();
    return 1;
}

void
usbn_detach(int port)
{
    if ((port < 0) || (port >= USBN_PORTS))
        return;
    bus_lock();
    if (plug[port]) {
        if (plug[port]->destroy)
            plug[port]->destroy(plug[port]);
        plug[port] = NULL;
    } else if (devs[port])
        unplug[port] = 1;
    bus_unlock();
    usbn_port_cfg[port][0] = '\0';
}

/* Plug back in what the config says was plugged in. */
void
usbn_restore_ports(void)
{
    for (int p = 0; p < USBN_PORTS; p++) {
        unsigned vid, pid;
        char     err[512];

        if (sscanf(usbn_port_cfg[p], "%x:%x", &vid, &pid) != 2)
            continue;
        usbn_device_t *d = usbn_host_open((uint16_t) vid, (uint16_t) pid, err, sizeof(err));
        if (d)
            usbn_attach(p, d);
        else
            pclog("USB: port %d: %04X:%04X not reattached: %s\n", p + 1, vid, pid, err);
    }
}
