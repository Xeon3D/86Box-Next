# What the NVIDIA Windows 9x drivers expect from an NV3

Reverse-engineered from the RIVA 128 Windows 98 driver set (NV3RM.VXD resource
manager, NV3DISP.DRV display driver, NV3DD32.DLL DirectDraw/D3D) while bringing up
this emulation. File offsets are into the files as shipped; NV3RM.VXD is an LE
VxD whose code object starts at file offset 0xAC00 (runtime address = file offset
+ a per-boot load delta). Table pointers in the VxD are relocation fixups (zero in
the file): resolve them through the LE fixup table.

## Resource manager (NV3RM.VXD)

### Objects, binding and lazy instantiation
- The resman keeps its own record per channel and subchannel of the object bound
  there (`[chan+0x2c + subch*0x40]`). It cannot see hardware binds (method 0 that
  hit RAMHT), so it **re-reads the PFIFO CACHE1 context registers 0x3280-0x32F0**
  (stride 0x10, one per subchannel) whenever it needs to (0x26950 "sync and zero",
  0x272EE "object changed"), looks each live context up by (class, instance)
  (0x422E4) and records it. A context register that reads back wrong desynchronises
  the resman from the driver (this was the mode-switch bug: subch 7 read as 0).
- A mode set reloads PFIFO state (0x24FB0): it clears all of RAMHT and the resman's
  RAMHT shadow, zeroes every subchannel context (after the sync above), and marks
  the instances of most graphics objects stale (`obj+0x334 &= 0xffff0000`).
- From then on objects are re-instantiated lazily:
  - a driver bind (method 0) misses RAMHT -> PFIFO CACHE_ERROR (hash failure) ->
    the resman's bind handler (0x19D00) records the handle for the subchannel and
    writes context 0 (PULL_STATE bit 4 "dirty" set);
  - the next method on that subchannel traps as a software method (context bit 23
    clear) -> the resman instantiates the object (writes its RAMIN words,
    re-inserts its RAMHT entry), writes the real context into CACHE1 and **leaves
    the method queued** so the hardware re-executes it.
  - Objects that are never put in RAMHT (the readback M2MF, handle 0x1020) always
    take this path.
- While a subchannel's context is 0, the resman executes that subchannel's methods
  in software through its class tables (dispatcher 0x19BF6): method found in the
  recorded object's class table -> handler(obj, entry, method, data).
- PFIFO cache-error handler: 0x27EB6. It reads the entry at GET (0x3300 + GET*2,
  method at +0, data at +4; 0x3400 for 64-entry caches), handles methods < 0x100
  through a small table (0x0-0x3 bind -> 0x19D00, 0x20-0x2C, 0x30-0x37, 0x38-0x4B),
  others through the object's class table; then keeps going while the next
  entries also belong to software objects, advancing GET (0x3270) itself.

### Object options word (RAMIN word 0)
- Created objects get options 0xFFFFFFFF ("not validated", 0x2E6E4); the
  validator 0x32D5B computes them from the object's colour format (+0x33C, -1 =
  unset), the patch (+0x340, +0x348, surface +0x3AC ...) and writes them to RAMIN
  and every CTX_CACHE entry bound to it (0x371AD -> 0x429A3, 0x272EE).
- Image classes (IFC, SIFC, bitmap, ...) route their data methods (0x304, 0x400+)
  through 0x371AD in software, which validates and commits the object first.

### PGRAPH interrupt handler (0x2D2B0)
- Reads INTR (0x400100 & 0x11111011), INVALID (0x400104), CTX_SWITCH (0x400180),
  TRAPPED_ADDR (0x4006B4: chid 30:24, class 20:16, subch 15:13, method 12:2),
  TRAPPED_DATA (0x4006B8), TRAPPED_INSTANCE (0x4006BC) and looks the object up by
  (chid, class|0x40, instance).
- Services: INVALID with INVALID_METHOD (software methods: 0x100, 0x104 notify,
  0x200, 0x300...), MISSING_METHOD (bit 16: error report, writes 0x400508=0x33),
  NOTIFY (bit 28), CONTEXT_SWITCH (bit 4, load routine near 0x42040 in the file).
  XY_RANGE, MISSING_FORMAT and CLIP_SOFTWARE are ignored.
- Waits for PGRAPH idle: polls 0x4006B0 == 0; FIFO access 0x4006A4.
- PDMA faults (0x401100) are only acknowledged.

### M2MF (class 0x0D)
- Class method table (file 0xE130): 0x100, 0x104 notify, 0x108, 0x300 (0x5A979),
  0x304 (0x5AB3A), 0x308 notify DMA (0x5ACFB, field +0x350), 0x30C-0x32B (0x5ADAB
  -> commit 0x5A579).
- Commit 0x5A579 checks the three DMA objects (+0x34C, +0x348, +0x350) with
  0x1E3FC (non-null, instantiated, range) and instantiates them (0x1E480), sets
  word 2 bit 16 when the source DMA object is the framebuffer (+0x480 == 3 type),
  writes 0x401400 = DMA limit, then binds. Any failure = transfer dropped,
  notifier never written.

### DMA objects
- Global list `[dev+0x4586]`, next at +0x484, handle +0, channel +0x47C; lookup
  0x1E36E, create 0x1DF40, destroy 0x1E072.
- Instance written by 0x1EFAD: word 0 = page offset (11:0) | 0x10000 (page table)
  | target (0x2000000 PCI, 0x3000000 AGP, type from +0x480), word 1 = limit, then
  one PTE per page (address | 3), terminated by 0xFFFFFFFF.
- Instance heap: bitmap allocator 0x22DED in 16-byte units, base `[dev+0x11C]`,
  size `[dev+0x120]`; returns 0x1000106 when full.

### Instance memory layout (state load 0x21530, size from 0x22221)
- Instance memory = VRAM - (bpp/8 * width * height * buffers), capped at 64 KB,
  plus 0x3000 and 0x5000 (special-cased 960x720 / 1920x1080) -> normally 96 KB at
  the top of VRAM.
- >= 64 KB: RAMHT 16 KB at 0 (0x2210 = 0x20000), RAMRO 0x4000 (0x2218), RAMFC
  0x4200 (0x2214), heap from 0x4600.
  Smaller: RAMHT 8 KB (0x10000) / RAMRO 0x2000 / RAMFC 0x2200 / heap 0x2600, or
  RAMHT 4 KB / RAMRO 0x1000 / RAMFC 0x1200 / heap 0x1600.
- RAMHT hash: ((h ^ h>>8 ^ h>>16 ^ h>>24) & 0xFF) ^ chid; 8 bytes per entry
  (handle, context), `[dev+0x110]` entries per bucket.

### Class method tables
Every class the resman knows has a table of (handler, first method, last
method) triples between file offsets 0xD6C0 and 0xE4A0 (pointers via fixups).
These list exactly which methods each class accepts (a method outside its table
is an error), and which handler runs when the method is executed in software.

## Display driver (NV3DISP.DRV)
- Subchannel use: 0 surfaces/rop (0x31...), 2 the destination surface (handles
  0x1F00/0x1F05, rebinds constantly), 3 ROP/pattern/patchcords, 4 image (0x11),
  5 text (0x0C), 6 rectangles, **7 shared**: blit 0x1700, stretched image 0x1B00,
  readback M2MF 0x1020. The driver caches what it bound to subch 7 in one dword
  (`[0x440188]` in the 32-bit part, also used by the 16-bit BitBlt at 0x17A8)
  and only binds when it differs.
- GetPixel (0x5BA07): sets the notifier status byte to 0xFF, programs the M2MF
  readback (0x5B3C0: bind 0x1020 if needed, OFFSET_IN = y*pitch + x*2, pitch,
  length, count 1, format 0x101, BUFFER_NOTIFY), then spins on the status byte.
- Free FIFO space read from USER +0x10 of the subchannel (e.g. 0xE010) before
  each burst.
- Object setup at init (0x6F380, 0x70C20): binds and patchcord/DMA setup through
  subchannel 3 software objects (methods 0x300/0x304 with object handles).
