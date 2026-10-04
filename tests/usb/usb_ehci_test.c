/*
 * 86Box-Next  Tests for the USB 2.0 card (src/usb/usb_ehci.c): the EHCI
 *             controller and its UHCI companion sharing two ports.
 *
 *             Against fake guest memory, PCI, MMIO, I/O, timers and mutexes
 *             (usb_test_fakes.c) with fake devices, the tests do what a
 *             guest's EHCI driver does -- take the ports with CONFIGFLAG,
 *             reset them, hand a full-speed device to the companion -- and
 *             run control, short and stalled transfers through the
 *             asynchronous schedule, checked against EHCI 1.0.
 *
 *             No test framework: build with CTest (BUILD_TESTING=ON).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/usb_next.h>
#include "usb_test_fakes.h"

extern const device_t usb_ehci_via_device;

static fake_dev_t hs, fs;

#define OP          0x20
#define USBCMD      (OP + 0x00)
#define USBSTS      (OP + 0x04)
#define USBINTR     (OP + 0x08)
#define FRINDEX     (OP + 0x0c)
#define ASYNC       (OP + 0x18)
#define CONFIGFLAG  (OP + 0x40)
#define PORTSC(p)   (OP + 0x44 + 4 * (p))

#define QH   0x20000
#define QTD  0x20100
#define BUF  0x22000

#define ACTIVE  0x80
#define HALTED  0x40

static void
qtd(uint32_t at, uint32_t next, uint32_t alt, int pid, int bytes, int ioc, uint32_t buf)
{
    wr32m(at, next);
    wr32m(at + 4, alt);
    wr32m(at + 8, ACTIVE | (pid << 8) | (3 << 10) | (ioc ? 0x8000 : 0) | ((uint32_t) bytes << 16));
    wr32m(at + 0x0c, buf);
    for (int i = 1; i < 5; i++)
        wr32m(at + 0x0c + i * 4, (buf & ~0xfff) + i * 0x1000);
}

/* One queue head, alone in the async ring (its own next, H set). */
static void
qh(uint8_t addr, uint8_t ep, int maxp, uint32_t first)
{
    wr32m(QH, QH | 2);
    wr32m(QH + 4, addr | (ep << 8) | (2 << 12) | (1 << 14) | (1 << 15) | ((uint32_t) maxp << 16));
    wr32m(QH + 8, 0x40000000);
    wr32m(QH + 0x0c, 0);
    wr32m(QH + 0x10, first);   /* overlay: next qTD */
    wr32m(QH + 0x14, 1);
    wr32m(QH + 0x18, 0);       /* overlay token: inactive, so the first qTD loads */
}

static void
test_pci(void)
{
    CHECK(fake_pci_rd(1, 0, 1, fake_pci_priv) == 0x06 && fake_pci_rd(1, 2, 1, fake_pci_priv) == 0x04
              && fake_pci_rd(1, 3, 1, fake_pci_priv) == 0x31,
          "function 1 is the VIA VT6202 EHCI");
    CHECK(fake_pci_rd(1, 9, 1, fake_pci_priv) == 0x20 && fake_pci_rd(1, 0x0a, 1, fake_pci_priv) == 0x03,
          "class USB, programming interface EHCI");
    CHECK(fake_pci_rd(0, 9, 1, fake_pci_priv) == 0x00 && fake_pci_rd(0, 0x0e, 1, fake_pci_priv) == 0x80,
          "function 0 is the UHCI companion of a multi-function card");
    CHECK(fake_pci_rd(2, 0, 1, fake_pci_priv) == 0xff, "no function 2");
    CHECK(fake_pci_rd(1, 0x60, 1, fake_pci_priv) == 0x20, "SBRN says USB 2.0");

    fake_pci_wr(1, 0x13, 1, 0xfe, fake_pci_priv);
    fake_pci_wr(1, 0x04, 1, 0x06, fake_pci_priv);
    CHECK(fake_mmio_base == 0xfe000000, "BAR0 maps the registers (%08X)", fake_mmio_base);
    fake_pci_wr(0, 0x21, 1, 0xe0, fake_pci_priv);
    fake_pci_wr(0, 0x04, 1, 0x05, fake_pci_priv);
    CHECK(fake_io_base == 0xe000, "the companion's registers at E000");

    CHECK((fake_mmio_rl(0) & 0xff) == 0x20 && (fake_mmio_rl(0) >> 16) == 0x0100, "CAPLENGTH 0x20, HCIVERSION 1.00");
    uint32_t hcs = fake_mmio_rl(4);
    CHECK((hcs & 0xf) == 2 && ((hcs >> 12) & 0xf) == 1, "two ports, one companion (%08X)", hcs);
    CHECK(fake_mmio_rl(USBSTS) & 0x1000, "halted after reset");
    CHECK(usbn_bus_high_speed(), "the USB 2.0 card takes high-speed devices");
}

static void
test_ownership(void)
{
    usbn_attach(0, &hs.dev);
    usbn_attach(1, &fs.dev);
    fake_frames(1);

    CHECK(fake_mmio_rl(PORTSC(0)) & 0x2000, "before CONFIGFLAG the ports are the companion's");
    CHECK(!(fake_mmio_rl(PORTSC(0)) & 1), "so EHCI sees nothing on port 1");
    CHECK(fake_io_rw(0x10) & 1, "the companion shows the high-speed device (a guest without EHCI drivers)");
    CHECK(hs.dev.fs_view, "at full speed");
    CHECK(fake_io_rw(0x12) & 1, "but it does see the full-speed one on port 2");

    fake_mmio_wl(CONFIGFLAG, 1);
    uint32_t p0 = fake_mmio_rl(PORTSC(0));
    CHECK(!(p0 & 0x2000) && (p0 & 3) == 3, "CONFIGFLAG: port 1 is EHCI's, connected, change bit set (%08X)", p0);
    CHECK(((p0 >> 10) & 3) == 2, "high/full speed device idles in the J state");
    CHECK(fake_mmio_rl(USBSTS) & 4, "port change detect");
    CHECK(!(fake_io_rw(0x12) & 1), "the companion lost port 2 to EHCI");
    CHECK(!(fake_io_rw(0x10) & 1) && !hs.dev.fs_view, "port 1 moved to EHCI, at high speed again");

    /* Reset port 1: a high-speed device comes out enabled. */
    fake_mmio_wl(PORTSC(0), 0x1000 | 0x100 | 2);
    fake_mmio_wl(PORTSC(0), 0x1000);
    p0 = fake_mmio_rl(PORTSC(0));
    CHECK((p0 & 4) && !(p0 & 0x100), "high-speed device enabled after reset (%08X)", p0);
    CHECK(hs.resets == 1, "the reset reached the device");

    /* Reset port 2: the full-speed device does not, so the driver gives it
       to the companion. */
    fake_mmio_wl(PORTSC(1), 0x1000 | 0x100 | 2);
    fake_mmio_wl(PORTSC(1), 0x1000);
    uint32_t p1 = fake_mmio_rl(PORTSC(1));
    CHECK(!(p1 & 4), "full-speed device stays disabled (%08X)", p1);
    fake_mmio_wl(PORTSC(1), 0x1000 | 0x2000);
    CHECK(!(fake_mmio_rl(PORTSC(1)) & 1), "EHCI port 2 disconnected after PORT_OWNER");
    CHECK(fake_io_rw(0x12) & 1, "the companion has the full-speed device");
    fake_mmio_wl(USBSTS, 0x3f);
}

static void
test_control_transfer(void)
{
    static const uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
    memcpy(ram + BUF, setup, 8);
    memset(ram + BUF + 0x100, 0, 64);

    qtd(QTD + 0x00, QTD + 0x20, 1, 2, 8, 0, BUF);              /* SETUP */
    qtd(QTD + 0x20, QTD + 0x40, 1, 1, 18, 0, BUF + 0x100);     /* IN 18 */
    qtd(QTD + 0x40, 1, 1, 0, 0, 1, 0);                         /* status OUT, IOC */
    qh(0, 0, 64, QTD);

    hs.naks = 2;
    fake_mmio_wl(USBINTR, 0x3f);
    fake_mmio_wl(ASYNC, QH);
    fake_mmio_wl(USBCMD, 0x00080000 | 0x20 | 1);   /* run, async enabled */
    CHECK(!(fake_mmio_rl(USBSTS) & 0x1000), "running");
    CHECK(fake_mmio_rl(USBSTS) & 0x8000, "async schedule status follows ASE");

    fake_frames(1);
    CHECK(!(rd32m(QTD + 8) & ACTIVE), "SETUP done");
    CHECK(rd32m(QTD + 0x28) & ACTIVE, "IN still active after a NAK");
    CHECK(!fake_irq[2], "no interrupt yet");

    fake_frames(3);
    uint32_t in = rd32m(QTD + 0x28);
    CHECK(!(in & ACTIVE) && ((in >> 16) & 0x7fff) == 0, "IN done, all 18 bytes moved (%08X)", in);
    CHECK(!memcmp(ram + BUF + 0x100, fake_dev_desc, 18), "descriptor in guest memory");
    CHECK(!(rd32m(QTD + 0x48) & ACTIVE), "status stage done");
    CHECK(fake_mmio_rl(USBSTS) & 1, "USBINT from the IOC qTD");
    CHECK(fake_irq[2], "INTB asserted");
    fake_mmio_wl(USBSTS, 0x3f);
    CHECK(!fake_irq[2], "INTB drops when the status is cleared");
    CHECK(fake_mmio_rl(FRINDEX) == 4 * 8, "FRINDEX counts microframes (%u)", fake_mmio_rl(FRINDEX));
}

static void
test_short_and_stall(void)
{
    hs.dev.addr = 5;

    /* A short IN takes the alternate next qTD. */
    qtd(QTD + 0x00, QTD + 0x20, QTD + 0x40, 1, 64, 0, BUF + 0x200);
    qtd(QTD + 0x20, 1, 1, 1, 64, 0, BUF + 0x300);
    qtd(QTD + 0x40, 1, 1, 1, 64, 1, BUF + 0x400);
    qh(5, 2, 512, QTD);
    fake_frames(1);
    uint32_t t = rd32m(QTD + 8);
    CHECK(!(t & ACTIVE) && ((t >> 16) & 0x7fff) == 62, "short packet: 2 of 64 bytes, 62 left (%08X)", t);
    CHECK(ram[BUF + 0x200] == 0xaa && ram[BUF + 0x201] == 0x55, "short packet data");
    CHECK(rd32m(QTD + 0x28) & ACTIVE, "the next qTD was skipped");
    CHECK(!(rd32m(QTD + 0x48) & ACTIVE), "the alternate next qTD ran");
    fake_mmio_wl(USBSTS, 0x3f);

    /* A stall halts the queue. */
    qtd(QTD, 1, 1, 0, 4, 0, BUF);
    qh(5, 1, 512, QTD);
    fake_frames(1);
    t = rd32m(QTD + 8);
    CHECK(!(t & ACTIVE) && (t & HALTED), "STALL halts the qTD (%08X)", t);
    CHECK(rd32m(QH + 0x18) & HALTED, "and the queue head's overlay");
    CHECK(fake_mmio_rl(USBSTS) & 2, "error interrupt status");
    fake_mmio_wl(USBSTS, 0x3f);
}

static void
test_doorbell(void)
{
    fake_mmio_wl(USBCMD, fake_mmio_rl(USBCMD) | 0x40);
    fake_frames(1);
    CHECK(!(fake_mmio_rl(USBCMD) & 0x40), "doorbell cleared by the controller");
    CHECK(fake_mmio_rl(USBSTS) & 0x20, "interrupt on async advance");
    fake_mmio_wl(USBSTS, 0x3f);
}

static void
test_unplug_and_release(void)
{
    usbn_detach(0);
    fake_frames(1);
    uint32_t p0 = fake_mmio_rl(PORTSC(0));
    CHECK(!(p0 & 1) && (p0 & 2) && !(p0 & 4), "unplugged: disconnected, change bit, disabled (%08X)", p0);
    CHECK(hs.destroyed == 1, "the device is released");

    fake_mmio_wl(CONFIGFLAG, 0);
    CHECK(fake_mmio_rl(PORTSC(1)) & 0x2000, "CONFIGFLAG cleared: ports back to the companion");
    CHECK(fake_io_rw(0x12) & 1, "the companion still has the full-speed device");
}

int
main(void)
{
    fake_dev_init(&hs, "high-speed", USBN_SPEED_HIGH);
    fake_dev_init(&fs, "full-speed", USBN_SPEED_FULL);
    void *dev = usb_ehci_via_device.init(&usb_ehci_via_device);

    test_pci();
    test_ownership();
    test_control_transfer();
    test_short_and_stall();
    test_doorbell();
    test_unplug_and_release();
    usb_ehci_via_device.close(dev);
    CHECK(fs.destroyed == 1, "closing the card releases what was plugged in");

    printf("EHCI: %d checks, %d failed\n", fake_checks, fake_failures);
    return fake_failures ? 1 : 0;
}
