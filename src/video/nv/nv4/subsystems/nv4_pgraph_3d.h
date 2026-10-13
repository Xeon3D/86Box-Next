/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          NVIDIA RIVA TNT / TNT2 -- the Direct3D classes' batch: draw
 *          states, triangles and spans, as the CPU span shader
 *          (nv4_pgraph_3d.c) and the GPU lane (nv4_pgraph_3d_gpu.c)
 *          consume them.
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves
 */
#ifndef NV4_PGRAPH_3D_H
#define NV4_PGRAPH_3D_H

struct nv4_gpu_s;

#define B3D_STATES 64
#define B3D_TRIS   16384
#define B3D_SPANS  262144

/* Attributes interpolated across a triangle */
enum {
    AT_Z,
    AT_Q,       /* rhw, or 1 without perspective */
    AT_U0,      /* u * q */
    AT_V0,
    AT_U1,
    AT_V1,
    AT_R,
    AT_G,
    AT_B,
    AT_A,
    AT_SR,
    AT_SG,
    AT_SB,
    AT_FOG,
    AT_COUNT
};

typedef struct {
    int      on;
    uint32_t dma;      /* DMA object instance */
    uint32_t offset;   /* level 0 */
    int      fmt;      /* texel format 1-7 */
    int      bpp;
    int      wlog, hlog;
    int      levels;
    uint32_t level_off[12];
    int      wrap_u, wrap_v;
    int      min, mag;
    float    lod_bias;
    int      center;   /* texel centers at +0.5 */
    int      ckey;
    uint32_t size;     /* bytes of all levels */
} nv4_3d_tex_t;

typedef struct {
    int      dx6;
    uint32_t col_base, z_base;     /* absolute VRAM byte addresses */
    uint32_t col_pitch, z_pitch;
    uint32_t col_lim, z_lim;       /* last valid byte, absolute */
    int      col_fmt, bpp;
    int      z_on;                 /* a depth surface exists */
    int32_t  cx0, cy0, cx1, cy1;
    float    origin;               /* 0: D3D pixel centers, 0.5: corner */
    nv4_3d_tex_t tex[2];
    uint32_t color_key;
    /* DX5 */
    int      texblend;
    /* DX6 */
    uint32_t comb_a[2], comb_c[2], factor;
    /* both */
    int      flat, persp, spec, fog, ablend, sblend, dblend;
    uint32_t fog_color;
    int      atest, afunc, aref;
    int      zen, zfunc, zwrite, zfloat;
    int      cull, dither;
    int      wr_a, wr_r, wr_g, wr_b;
    int      sten, sfunc, sref, smask_r, smask_w, sop_fail, sop_zfail, sop_zpass, swrite;
} nv4_3d_state_t;

typedef struct {
    int   state;
    float x0, y0;            /* vertex 0, pixels */
    float c[AT_COUNT], dx[AT_COUNT], dy[AT_COUNT];
} nv4_3d_tri_t;

typedef struct {
    int16_t  y, x0, x1;      /* x1 exclusive */
    uint16_t tri;
} nv4_3d_span_t;

#define B3D_MAX_WORKERS 8

typedef struct {
    nv4_t    *nv4;
    int       id;
    thread_t *thread;
    event_t  *wake; /* a batch is ready, or the pool is closing */
    event_t  *done; /* this worker finished the batch */
} nv4_3d_worker_t;

typedef struct nv4_3d_batch_s {
    nv4_3d_state_t states[B3D_STATES];
    nv4_3d_tri_t   tris[B3D_TRIS];
    nv4_3d_span_t  spans[B3D_SPANS];
    int            nstates, ntris, nspans;

    /* Row layout every state of the batch shares (threaded only) */
    int      rt_valid;
    uint32_t rt_col_base, rt_col_pitch, rt_z_base, rt_z_pitch;
    int      rt_bpp, rt_z_on;
    int      serial; /* rows alias: shade on one thread */
    /* VRAM the batch writes, color and depth kept apart (depth also
       when only read) */
    int      wr_valid;
    uint32_t wr_c_lo, wr_c_hi, wr_z_lo, wr_z_hi;

    int             nthreads;
    int             run;
    nv4_3d_worker_t workers[B3D_MAX_WORKERS];

    /* Rows are shaded apart (threads or the GPU): the batch must keep
       every row on its own bytes */
    int               parallel;
    struct nv4_gpu_s *gpu;

    uint64_t st_flushes, st_spans, st_us; /* for nv4_3d_stats */
    uint64_t st_sync_us[NV4_SYNC_CAUSES]; /* time waiting for the GPU, by cause */
    uint32_t st_sync_n[NV4_SYNC_CAUSES];

    /* NV4_3D_VERIFY */
    int      verify;
    uint8_t *vsnap, *vres;
    uint32_t vflushes, vbad;
} nv4_3d_batch_t;

/* The GPU lane (nv4_pgraph_3d_gpu.c). nv4_gpu_shade runs a whole batch
   and returns 1, or returns 0 and leaves VRAM untouched when it cannot
   take it; the caller then shades it on the CPU. */
extern struct nv4_gpu_s *nv4_gpu_init(nv4_t *nv4);
extern int               nv4_gpu_shade(struct nv4_gpu_s *g, nv4_t *nv4, const nv4_3d_batch_t *b);
extern void              nv4_gpu_close(struct nv4_gpu_s *g);
/* The batch in flight: wait for it and copy its rows into VRAM; does
   it touch bytes lo..hi? */
extern void              nv4_gpu_wait(struct nv4_gpu_s *g, nv4_t *nv4);
extern int               nv4_gpu_pending_overlaps(struct nv4_gpu_s *g, uint32_t lo, uint32_t hi);
extern void              nv4_gpu_counts(struct nv4_gpu_s *g, uint64_t *batches, uint64_t *fallbacks);

#endif
