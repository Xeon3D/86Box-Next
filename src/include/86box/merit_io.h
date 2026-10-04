/*
 * 86Box-Next: the Merit Megatouch I/O boards (from MegaPPBox) -- cabinet
 * controls.
 *
 * The coin mech and the two operator buttons are wires on the I/O board, not
 * keys, so the UI drives them here rather than typing into the guest.
 */
#ifndef EMU_MERIT_IO_H
#define EMU_MERIT_IO_H
#ifdef __cplusplus
extern "C" {
#endif

#define MERIT_LINE_COIN1     0 /* 0x22C bit 2, active high */
#define MERIT_LINE_COIN2     1 /* 0x22C bit 3 */
#define MERIT_LINE_COIN3     2 /* 0x22C bit 4 */
#define MERIT_LINE_COIN4     3 /* 0x22C bit 5 */
#define MERIT_LINE_SETUP     4 /* 0x22C bit 0, active low */
#define MERIT_LINE_CALIBRATE 5 /* 0x22C bit 1, active low */
#define MERIT_LINES          6

/* Assert a line for `ms` milliseconds of host time (0 = the line's default:
   100 ms for a coin, 500 ms for a button). Safe to call from the UI thread;
   does nothing when the board is not fitted. */
extern void merit_io_pulse(int line, int ms);
extern int  merit_io_present(void);

/* A guest CPU reset that is a reboot (CMOS shutdown byte 0) on the MAXX board:
   schedules a full machine reset and returns 1. */
extern int merit_io_reboot_as_hard_reset(void);

/* The DS1991 key socket. merit_io_set_key() fits the dump `fn` (NULL or "" =
   pull the key) at the board's next key-line access and saves it as the
   board's key_file; merit_io_key_file() is the fitted / requested dump ("" =
   none, NULL = no board). Both are for the UI thread. */
extern void        merit_io_set_key(const char *fn);
extern const char *merit_io_key_file(void);

#ifdef EMU_DEVICE_H
extern const device_t merit_io_xl_device;
extern const device_t merit_io_maxx_device;
#endif

#ifdef __cplusplus
}
#endif
#endif
