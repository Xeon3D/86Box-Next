/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          86Box Guest Tools: the wire protocol between the emulator and
 *          the agents running inside the guest. Plain C with no
 *          dependencies: the guest agents (guest-tools/) include it too.
 *
 *          The channel is two I/O ports. Any width (8, 16 or 32 bits) works
 *          on both, so the same protocol serves an 8088 DOS driver and a
 *          32-bit Windows agent:
 *
 *            GT_PORT_CMD   read:  GT_MAGIC, byte n of it at GT_PORT_CMD + n
 *                          write: run a command (low byte) on the request
 *                                 bytes written so far, then start the reply
 *            GT_PORT_DATA  write: append bytes to the request
 *                          read:  next bytes of the reply (0 once past it)
 *
 *          Every reply starts with a 32-bit status (GT_OK or GT_ERR_*).
 *          All values are little-endian. The ports work from any privilege
 *          level: the emulator lets ring 3 and V86 code reach them whatever
 *          IOPL and the TSS bitmap say, so no guest kernel driver is needed.
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves.
 */
#ifndef EMU_GUEST_TOOLS_PROTO_H
#define EMU_GUEST_TOOLS_PROTO_H

#define GT_PORT_BASE      0x86b0
#define GT_PORT_CMD       (GT_PORT_BASE + 0)
#define GT_PORT_DATA      (GT_PORT_BASE + 4)
#define GT_PORT_COUNT     8

#define GT_MAGIC          0x58423638UL /* "86BX" */
#define GT_PROTO_VERSION  1

/* Commands (written to GT_PORT_CMD). */
#define GT_CMD_HELLO      0x01 /* req: u16 proto, u16 os, u32 agent caps
                                  reply: u32 status, u16 proto, u16 0, u32 host features */
#define GT_CMD_POLL       0x02 /* req: u32 agent state
                                  reply: u32 status, u32 flags, u16 x, u16 y, u32 clip seq */
#define GT_CMD_CLIP_GET   0x03 /* reply: u32 status, u32 format, u32 bytes, data */
#define GT_CMD_CLIP_SET   0x04 /* req: u32 format, u32 bytes, data; reply: u32 status */
#define GT_CMD_BYE        0x05 /* the agent is going away; reply: u32 status */

/* Files. Host to guest: CLIP_GET answers GT_CLIP_FILES with the list (see
   below), taken when the agent asked; FILE_READ reads an entry of it.
   Guest to host: FILES_BEGIN, a FILE_ADD per entry (directories before
   what they hold) with FILE_WRITEs for a file's contents, FILES_END; on a
   commit the host clipboard gets the entries without a '\' in their name. */
#define GT_CMD_FILE_READ  0x06 /* req: u32 index, u64 offset, u32 bytes (<= GT_FILE_CHUNK)
                                  reply: u32 status, u32 bytes, data */
#define GT_CMD_FILES_BEGIN 0x07 /* req: u32 entries, u64 total bytes; reply: u32 status */
#define GT_CMD_FILE_ADD   0x08 /* req: u32 flags, u64 size, u32 name bytes, name
                                  reply: u32 status, u32 index */
#define GT_CMD_FILE_WRITE 0x09 /* req: u32 index, u64 offset, u32 bytes, data; reply: u32 status */
#define GT_CMD_FILES_END  0x0a /* req: u32 action (GT_FILES_*); reply: u32 status */

/* Drag and drop. Host to guest: while the host drags files over the guest
   display, POLL says GT_POLL_DRAG (with a new drag seq for each drag);
   DRAG_GET answers as CLIP_GET does with GT_CLIP_FILES, and FILE_READ reads
   its entries with GT_FILE_DRAG_INDEX set in the index. GT_POLL_DROP: the
   host dropped them where the pointer is; neither flag: the drag left.
   Guest to host: GT_POLL_DRAG_OUT says the host pointer left the display
   with the left button down; an agent that catches a guest drag of files
   there says GT_STATE_DRAG_OUT and sends them with FILES_END GT_FILES_DRAG,
   and the host carries on the drag on its side. */
#define GT_CMD_DRAG_GET   0x0b /* reply: as CLIP_GET */

/* A line for the emulator's log, for debugging agents. */
#define GT_CMD_LOG        0x0c /* req: ASCII text; reply: u32 status */

/* Reply status. */
#define GT_OK             0
#define GT_ERR_COMMAND    1 /* unknown command */
#define GT_ERR_REQUEST    2 /* request too short or malformed */
#define GT_ERR_DISABLED   3 /* the feature is switched off on the host */
#define GT_ERR_NO_AGENT   4 /* HELLO first */
#define GT_ERR_TOO_BIG    5 /* more than GT_FILES_MAX_* */
#define GT_ERR_IO         6 /* the host could not read or write a file */

/* Guest operating systems (HELLO). */
#define GT_OS_UNKNOWN     0
#define GT_OS_DOS         1
#define GT_OS_WIN16       2
#define GT_OS_WIN9X       3
#define GT_OS_WINNT       4
#define GT_OS_LINUX       5
#define GT_OS_OS2         6

/* Agent capabilities (HELLO). */
#define GT_CAP_POINTER    0x0001 /* moves the guest pointer to the host's */
#define GT_CAP_CLIPBOARD  0x0002 /* exchanges clipboard text */
#define GT_CAP_FILES      0x0004 /* takes GT_CLIP_FILES, sends files */

/* Host features (HELLO reply). */
#define GT_FEAT_POINTER   0x0001 /* pointer integration is on */
#define GT_FEAT_CLIP_IN   0x0002 /* host clipboard -> guest */
#define GT_FEAT_CLIP_OUT  0x0004 /* guest clipboard -> host */
#define GT_FEAT_FILES     0x0008 /* files go with the clipboard, both ways it is on */

/* Agent state (POLL request). */
#define GT_STATE_POINTER  0x0001 /* the agent follows the host pointer right now */
#define GT_STATE_DRAG_OUT 0x0002 /* the agent caught a guest drag leaving the display */

/* POLL reply flags. */
#define GT_POLL_POINTER   0x0001 /* pointer integration active: x, y are valid */
#define GT_POLL_INSIDE    0x0002 /* the host pointer is over the guest display */
#define GT_POLL_CLIP      0x0004 /* the host clipboard has news (see clip seq) */
#define GT_POLL_DRAG      0x0008 /* the host drags files over the display (see drag seq) */
#define GT_POLL_DROP      0x0010 /* ... and dropped them */
#define GT_POLL_DRAG_OUT  0x0020 /* the host pointer left the display, left button down */
/* The POLL reply has a u32 drag seq after the clip seq. */

/* Pointer coordinates: 0..GT_POINTER_MAX across the guest display. */
#define GT_POINTER_MAX    0xffff

/* Clipboard formats. */
#define GT_CLIP_NONE      0
#define GT_CLIP_TEXT      1 /* UTF-16LE, LF line ends, no terminator */
#define GT_CLIP_FILES     2 /* u32 entries, then per entry: u32 flags, u64 size,
                               u32 name bytes, name (UTF-16LE, a path relative to
                               the copied items' folder, '\' between its parts) */
#define GT_CLIP_MAX_BYTES (1024UL * 1024UL)

/* File entries. */
#define GT_FILE_DIR       0x0001

/* FILE_READ index of an entry in the drag's list (DRAG_GET). */
#define GT_FILE_DRAG_INDEX 0x80000000UL

#define GT_FILE_CHUNK     65536UL
#define GT_FILES_MAX_ENTRIES 20000UL
#define GT_FILES_MAX_BYTES   (2048ULL * 1024ULL * 1024ULL)

/* FILES_END actions. */
#define GT_FILES_CANCEL   0 /* drop what came */
#define GT_FILES_CLIPBOARD 1 /* put it on the host clipboard */
#define GT_FILES_DRAG     2 /* the files of the guest drag leaving the display */

/* An agent that misses POLLs for this long is gone. */
#define GT_AGENT_TIMEOUT_MS 2000

#endif /* EMU_GUEST_TOOLS_PROTO_H */
