/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          NVIDIA RIVA TNT / TNT2 -- GPU lane for the Direct3D classes.
 *
 *          Optional, off by default: the "Draw 3D on the host GPU"
 *          option, or NV4_GPU=1 in the environment. A flushed batch
 *          (nv4_pgraph_3d.h) is shaded by a Vulkan compute kernel
 *          (nv4_pgraph_3d_gpu.comp) instead of the CPU: setup stays on
 *          the CPU, the kernel takes the same draw states, triangles
 *          and spans, one workgroup per framebuffer row walking its
 *          spans in submission order, and writes the bytes the CPU
 *          span shader would.
 *
 *          A flush is synchronous. The color and depth rows the batch
 *          touches are copied into a device-local mirror of VRAM, the
 *          batch's textures are gathered through their DMA objects
 *          (VRAM or system memory alike) into a texture arena, the
 *          kernel runs, and the rows are copied back. A batch the lane
 *          cannot take (rows past the end of VRAM, textures beyond the
 *          arena) is left to the CPU.
 *
 *          Vulkan is opened at run time, so a host without it only
 *          logs that the lane is off. NV4_3D_VERIFY=1 compares every
 *          batch with the CPU's result (nv4_pgraph_3d.c).
 *
 * Authors: Marcos Alves, <xeon4d@gmail.com>
 *
 *          Copyright 2026 Marcos Alves
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/dma.h>
#include <86box/mem.h>
#include <86box/pci.h>
#include <86box/rom.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/video.h>
#include <86box/vid_svga.h>
#include <86box/nv/vid_nv4.h>
#include "nv4_pgraph_3d.h"

#ifndef NV4_GPU_HAVE_VULKAN

struct nv4_gpu_s *
nv4_gpu_init(UNUSED(nv4_t *nv4))
{
    pclog("NV4 GPU: built without Vulkan headers, 3D stays on the CPU\n");
    return NULL;
}

int
nv4_gpu_shade(UNUSED(struct nv4_gpu_s *g), UNUSED(nv4_t *nv4), UNUSED(const nv4_3d_batch_t *b))
{
    return 0;
}

void
nv4_gpu_close(UNUSED(struct nv4_gpu_s *g))
{
}

void
nv4_gpu_counts(UNUSED(struct nv4_gpu_s *g), uint64_t *batches, uint64_t *fallbacks)
{
    *batches   = 0;
    *fallbacks = 0;
}

#else

#    define VK_NO_PROTOTYPES
#    include <vulkan/vulkan.h>
#    ifdef _WIN32
#        include <windows.h>
#    else
#        include <dlfcn.h>
#    endif
#    include <86box/nv/nv4_3d_gpu_spv.h>

#    define GPU_TEX_ARENA (32u << 20)

/* Records as the kernel reads them (std430, every field 4 bytes). */
typedef struct {
    int32_t  on, fmt, bpp, wlog, hlog, levels, wrap_u, wrap_v, min, mag;
    float    lod_bias;
    int32_t  center, ckey;
    uint32_t base; /* level 0 in the texture arena */
    uint32_t level_off[12];
} nv4_gpu_tex_t;

typedef struct {
    uint32_t      col_base, z_base, col_pitch, z_pitch, col_lim, z_lim;
    int32_t       col_fmt, bpp, z_on, dx6;
    float         origin;
    uint32_t      color_key;
    int32_t       texblend;
    uint32_t      comb_a0, comb_a1, comb_c0, comb_c1, factor;
    int32_t       flat, persp, spec, fog, ablend, sblend, dblend;
    uint32_t      fog_color;
    int32_t       atest, afunc, aref, zen, zfunc, zwrite, zfloat, dither;
    int32_t       wr_a, wr_r, wr_g, wr_b;
    int32_t       sten, sfunc, sref, smask_r, smask_w, sop_fail, sop_zfail, sop_zpass, swrite;
    uint32_t      pad;
    nv4_gpu_tex_t tex[2];
} nv4_gpu_state_t;

typedef struct {
    int32_t y, x0, x1, tri;
} nv4_gpu_span_t;

typedef struct {
    uint32_t first, count;
} nv4_gpu_row_t;

_Static_assert(sizeof(nv4_gpu_tex_t) == 26 * 4, "nv4_gpu_tex_t layout");
_Static_assert(sizeof(nv4_gpu_state_t) == 100 * 4, "nv4_gpu_state_t layout");
_Static_assert(sizeof(nv4_3d_tri_t) == (3 + 3 * AT_COUNT) * 4, "nv4_3d_tri_t layout");
_Static_assert(AT_COUNT == 14, "the kernel interpolates 14 attributes");

#    define GPU_FNS(X)                              \
        X(vkEnumeratePhysicalDevices)               \
        X(vkGetPhysicalDeviceProperties)            \
        X(vkGetPhysicalDeviceQueueFamilyProperties) \
        X(vkGetPhysicalDeviceMemoryProperties)      \
        X(vkCreateDevice)                           \
        X(vkGetDeviceQueue)                         \
        X(vkCreateBuffer)                           \
        X(vkGetBufferMemoryRequirements)            \
        X(vkAllocateMemory)                         \
        X(vkBindBufferMemory)                       \
        X(vkMapMemory)                              \
        X(vkCreateDescriptorSetLayout)              \
        X(vkCreatePipelineLayout)                   \
        X(vkCreateShaderModule)                     \
        X(vkCreateComputePipelines)                 \
        X(vkDestroyShaderModule)                    \
        X(vkCreateDescriptorPool)                   \
        X(vkAllocateDescriptorSets)                 \
        X(vkUpdateDescriptorSets)                   \
        X(vkCreateCommandPool)                      \
        X(vkAllocateCommandBuffers)                 \
        X(vkCreateFence)                            \
        X(vkBeginCommandBuffer)                     \
        X(vkCmdBindPipeline)                        \
        X(vkCmdBindDescriptorSets)                  \
        X(vkCmdPushConstants)                       \
        X(vkCmdDispatch)                            \
        X(vkCmdPipelineBarrier)                     \
        X(vkCmdCopyBuffer)                          \
        X(vkEndCommandBuffer)                       \
        X(vkQueueSubmit)                            \
        X(vkWaitForFences)                          \
        X(vkResetFences)                            \
        X(vkResetCommandBuffer)                     \
        X(vkDeviceWaitIdle)                         \
        X(vkDestroyFence)                           \
        X(vkDestroyCommandPool)                     \
        X(vkDestroyDescriptorPool)                  \
        X(vkDestroyPipeline)                        \
        X(vkDestroyPipelineLayout)                  \
        X(vkDestroyDescriptorSetLayout)             \
        X(vkDestroyBuffer)                          \
        X(vkFreeMemory)                             \
        X(vkDestroyDevice)                          \
        X(vkDestroyInstance)

typedef struct {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void          *map; /* host-visible ones */
    VkDeviceSize   size;
} gpu_buf_t;

enum {
    B_VRAM,   /* device-local mirror of VRAM */
    B_TEX,    /* device-local texture arena */
    B_STATES, /* host-visible from here on */
    B_TRIS,
    B_SPANS,
    B_ROWS,
    B_VSTAGE, /* VRAM rows in and out */
    B_TSTAGE, /* textures in */
    B_COUNT
};

/* Textures gathered for the batch being shaded */
typedef struct {
    uint32_t dma, offset, size, base;
} gpu_texent_t;

typedef struct nv4_gpu_s {
    void                     *lib;
    PFN_vkGetInstanceProcAddr gipa;
#    define GPU_DECL(name) PFN_##name name;
    GPU_FNS(GPU_DECL)
#    undef GPU_DECL

    VkInstance            inst;
    VkPhysicalDevice      pd;
    VkDevice              dev;
    VkQueue               queue;
    uint32_t              qfam;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      pl;
    VkPipeline            pipe;
    VkDescriptorPool      dpool;
    VkDescriptorSet       dset;
    VkCommandPool         cpool;
    VkCommandBuffer       cb;
    VkFence               fence;
    gpu_buf_t             b[B_COUNT];

    uint32_t      vram_size;
    gpu_texent_t  texes[B3D_STATES * 2];
    int           ntex;
    uint32_t      tex_used;
    uint32_t      row_count[4096];
    uint32_t      row_first[4096];
    uint64_t      batches, fallbacks;
} nv4_gpu_t;

static void *
gpu_dl_open(const char *name)
{
#    ifdef _WIN32
    return (void *) LoadLibraryA(name);
#    else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#    endif
}

static void *
gpu_dl_sym(void *lib, const char *name)
{
#    ifdef _WIN32
    return (void *) GetProcAddress((HMODULE) lib, name);
#    else
    return dlsym(lib, name);
#    endif
}

static void
gpu_dl_close(void *lib)
{
#    ifdef _WIN32
    FreeLibrary((HMODULE) lib);
#    else
    dlclose(lib);
#    endif
}

static int
gpu_load(nv4_gpu_t *g)
{
    static const char *const cands[] = {
#    if defined(_WIN32)
        "vulkan-1.dll",
#    elif defined(__APPLE__)
        "libvulkan.1.dylib",
        "/opt/homebrew/lib/libvulkan.1.dylib",
        "/usr/local/lib/libvulkan.1.dylib",
        "libMoltenVK.dylib",
#    else
        "libvulkan.so.1",
        "libvulkan.so",
#    endif
    };

    for (size_t i = 0; !g->lib && (i < sizeof(cands) / sizeof(cands[0])); i++)
        g->lib = gpu_dl_open(cands[i]);
    if (!g->lib)
        return 0;
    g->gipa = (PFN_vkGetInstanceProcAddr) gpu_dl_sym(g->lib, "vkGetInstanceProcAddr");
    return g->gipa != NULL;
}

static uint32_t
gpu_mem_type(nv4_gpu_t *g, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;

    g->vkGetPhysicalDeviceMemoryProperties(g->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && ((mp.memoryTypes[i].propertyFlags & want) == want))
            return i;
    return UINT32_MAX;
}

static int
gpu_buf(nv4_gpu_t *g, gpu_buf_t *b, VkDeviceSize size, int host)
{
    VkBufferCreateInfo   bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements mr;

    bi.size  = size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g->vkCreateBuffer(g->dev, &bi, NULL, &b->buf) != VK_SUCCESS)
        return 0;
    g->vkGetBufferMemoryRequirements(g->dev, b->buf, &mr);
    ai.allocationSize = mr.size;
    if (host) {
        /* cached when there is such a type: the CPU reads rows back */
        ai.memoryTypeIndex = gpu_mem_type(g, mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                              | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX)
            ai.memoryTypeIndex = gpu_mem_type(g, mr.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } else
        ai.memoryTypeIndex = gpu_mem_type(g, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        return 0;
    if (g->vkAllocateMemory(g->dev, &ai, NULL, &b->mem) != VK_SUCCESS)
        return 0;
    if (g->vkBindBufferMemory(g->dev, b->buf, b->mem, 0) != VK_SUCCESS)
        return 0;
    if (host && (g->vkMapMemory(g->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS))
        return 0;
    b->size = size;
    return 1;
}

static void gpu_destroy(nv4_gpu_t *g);

static int
gpu_create(nv4_gpu_t *g)
{
    PFN_vkCreateInstance     create_instance;
    VkApplicationInfo        app  = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo     ici  = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice         pds[16];
    uint32_t                 npd  = 16;
    VkPhysicalDeviceProperties props;
    float                    prio = 1.0f;
    VkDeviceQueueCreateInfo  qci  = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo       dci  = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkQueueFamilyProperties  qf[16];
    uint32_t                 nqf  = 16;
    int                      best = -1;

    create_instance = (PFN_vkCreateInstance) g->gipa(NULL, "vkCreateInstance");
    if (!create_instance)
        return 0;
    app.pApplicationName = "86Box RIVA TNT";
    app.apiVersion       = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    if (create_instance(&ici, NULL, &g->inst) != VK_SUCCESS) {
        pclog("NV4 GPU: vkCreateInstance failed\n");
        return 0;
    }
#    define GPU_LOAD(name)                                          \
        g->name = (PFN_##name) g->gipa(g->inst, #name);             \
        if (!g->name) {                                             \
            pclog("NV4 GPU: no " #name "\n");                       \
            return 0;                                               \
        }
    GPU_FNS(GPU_LOAD)
#    undef GPU_LOAD

    if ((g->vkEnumeratePhysicalDevices(g->inst, &npd, pds) < 0) || !npd) {
        pclog("NV4 GPU: no Vulkan device\n");
        return 0;
    }
    /* a discrete GPU first, then an integrated one, then anything */
    for (int pass = 0; (pass < 3) && (best < 0); pass++)
        for (uint32_t i = 0; (i < npd) && (best < 0); i++) {
            g->vkGetPhysicalDeviceProperties(pds[i], &props);
            if ((pass == 0) && (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
                continue;
            if ((pass == 1) && (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU))
                continue;
            best = i;
        }
    g->pd = pds[best];
    g->vkGetPhysicalDeviceProperties(g->pd, &props);
    g->vkGetPhysicalDeviceQueueFamilyProperties(g->pd, &nqf, qf);
    g->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            g->qfam = i;
            break;
        }
    if (g->qfam == UINT32_MAX)
        return 0;
    qci.queueFamilyIndex     = g->qfam;
    qci.queueCount           = 1;
    qci.pQueuePriorities     = &prio;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;
    if (g->vkCreateDevice(g->pd, &dci, NULL, &g->dev) != VK_SUCCESS) {
        pclog("NV4 GPU: vkCreateDevice failed\n");
        return 0;
    }
    g->vkGetDeviceQueue(g->dev, g->qfam, 0, &g->queue);
    pclog("NV4 GPU: using %s\n", props.deviceName);

    if (!gpu_buf(g, &g->b[B_VRAM], g->vram_size, 0) || !gpu_buf(g, &g->b[B_TEX], GPU_TEX_ARENA, 0)
        || !gpu_buf(g, &g->b[B_STATES], sizeof(nv4_gpu_state_t) * B3D_STATES, 1)
        || !gpu_buf(g, &g->b[B_TRIS], sizeof(nv4_3d_tri_t) * B3D_TRIS, 1)
        || !gpu_buf(g, &g->b[B_SPANS], sizeof(nv4_gpu_span_t) * B3D_SPANS, 1)
        || !gpu_buf(g, &g->b[B_ROWS], sizeof(nv4_gpu_row_t) * 4096, 1)
        || !gpu_buf(g, &g->b[B_VSTAGE], g->vram_size, 1) || !gpu_buf(g, &g->b[B_TSTAGE], GPU_TEX_ARENA, 1)) {
        pclog("NV4 GPU: buffer allocation failed\n");
        return 0;
    }

    /* pipeline */
    {
        VkDescriptorSetLayoutBinding  lb[6];
        VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPushConstantRange           pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
        VkPipelineLayoutCreateInfo    pci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkShaderModuleCreateInfo      sci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        VkComputePipelineCreateInfo   cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        VkShaderModule                sm;
        VkDescriptorPoolSize          ps  = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 };
        VkDescriptorPoolCreateInfo    dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        VkDescriptorSetAllocateInfo   dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkDescriptorBufferInfo        dbi[6];
        VkWriteDescriptorSet          wr[6];
        VkResult                      res;

        for (int i = 0; i < 6; i++) {
            memset(&lb[i], 0, sizeof(lb[i]));
            lb[i].binding         = i;
            lb[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            lb[i].descriptorCount = 1;
            lb[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        lci.bindingCount = 6;
        lci.pBindings    = lb;
        if (g->vkCreateDescriptorSetLayout(g->dev, &lci, NULL, &g->dsl) != VK_SUCCESS)
            return 0;
        pci.setLayoutCount         = 1;
        pci.pSetLayouts            = &g->dsl;
        pci.pushConstantRangeCount = 1;
        pci.pPushConstantRanges    = &pcr;
        if (g->vkCreatePipelineLayout(g->dev, &pci, NULL, &g->pl) != VK_SUCCESS)
            return 0;
        sci.codeSize = sizeof(nv4_gpu_spv);
        sci.pCode    = (const uint32_t *) nv4_gpu_spv;
        if (g->vkCreateShaderModule(g->dev, &sci, NULL, &sm) != VK_SUCCESS)
            return 0;
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = sm;
        cpi.stage.pName  = "main";
        cpi.layout       = g->pl;
        res              = g->vkCreateComputePipelines(g->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &g->pipe);
        g->vkDestroyShaderModule(g->dev, sm, NULL);
        if (res != VK_SUCCESS) {
            pclog("NV4 GPU: pipeline creation failed (%d)\n", res);
            return 0;
        }
        dpi.maxSets       = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes    = &ps;
        if (g->vkCreateDescriptorPool(g->dev, &dpi, NULL, &g->dpool) != VK_SUCCESS)
            return 0;
        dai.descriptorPool     = g->dpool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts        = &g->dsl;
        if (g->vkAllocateDescriptorSets(g->dev, &dai, &g->dset) != VK_SUCCESS)
            return 0;
        for (int i = 0; i < 6; i++) {
            dbi[i].buffer = g->b[i].buf; /* B_VRAM .. B_ROWS are bindings 0-5 */
            dbi[i].offset = 0;
            dbi[i].range  = VK_WHOLE_SIZE;
            memset(&wr[i], 0, sizeof(wr[i]));
            wr[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet          = g->dset;
            wr[i].dstBinding      = i;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr[i].pBufferInfo     = &dbi[i];
        }
        g->vkUpdateDescriptorSets(g->dev, 6, wr, 0, NULL);
    }
    {
        VkCommandPoolCreateInfo     cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo           fci  = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

        cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = g->qfam;
        if (g->vkCreateCommandPool(g->dev, &cpci, NULL, &g->cpool) != VK_SUCCESS)
            return 0;
        cbai.commandPool        = g->cpool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (g->vkAllocateCommandBuffers(g->dev, &cbai, &g->cb) != VK_SUCCESS)
            return 0;
        if (g->vkCreateFence(g->dev, &fci, NULL, &g->fence) != VK_SUCCESS)
            return 0;
    }
    return 1;
}

struct nv4_gpu_s *
nv4_gpu_init(nv4_t *nv4)
{
    nv4_gpu_t *g = calloc(1, sizeof(nv4_gpu_t));

    g->vram_size = nv4->vram_size;
    if (!gpu_load(g)) {
        pclog("NV4 GPU: no Vulkan library, 3D stays on the CPU\n");
        free(g);
        return NULL;
    }
    if (!gpu_create(g)) {
        pclog("NV4 GPU: Vulkan setup failed, 3D stays on the CPU\n");
        gpu_destroy(g);
        return NULL;
    }
    return g;
}

static void
gpu_destroy(nv4_gpu_t *g)
{
    if (g->dev) {
        g->vkDeviceWaitIdle(g->dev);
        if (g->fence)
            g->vkDestroyFence(g->dev, g->fence, NULL);
        if (g->cpool)
            g->vkDestroyCommandPool(g->dev, g->cpool, NULL);
        if (g->dpool)
            g->vkDestroyDescriptorPool(g->dev, g->dpool, NULL);
        if (g->pipe)
            g->vkDestroyPipeline(g->dev, g->pipe, NULL);
        if (g->pl)
            g->vkDestroyPipelineLayout(g->dev, g->pl, NULL);
        if (g->dsl)
            g->vkDestroyDescriptorSetLayout(g->dev, g->dsl, NULL);
        for (int i = 0; i < B_COUNT; i++) {
            if (g->b[i].buf)
                g->vkDestroyBuffer(g->dev, g->b[i].buf, NULL);
            if (g->b[i].mem)
                g->vkFreeMemory(g->dev, g->b[i].mem, NULL);
        }
        g->vkDestroyDevice(g->dev, NULL);
    }
    if (g->inst && g->vkDestroyInstance)
        g->vkDestroyInstance(g->inst, NULL);
    if (g->lib)
        gpu_dl_close(g->lib);
    free(g);
}

void
nv4_gpu_counts(struct nv4_gpu_s *g, uint64_t *batches, uint64_t *fallbacks)
{
    *batches   = g->batches;
    *fallbacks = g->fallbacks;
}

void
nv4_gpu_close(struct nv4_gpu_s *g)
{
    if (!g)
        return;
    pclog("NV4 GPU: %llu batches on the GPU, %llu left to the CPU\n", (unsigned long long) g->batches,
          (unsigned long long) g->fallbacks);
    gpu_destroy(g);
}

/* Copy one texture's bytes, as the CPU span shader reads them through
   its DMA object, into the arena. Returns its arena offset, or
   UINT32_MAX when the arena is full. */
static uint32_t
gpu_gather_tex(nv4_gpu_t *g, nv4_t *nv4, const nv4_3d_tex_t *t)
{
    uint8_t *dst;
    uint32_t base;

    for (int i = 0; i < g->ntex; i++)
        if ((g->texes[i].dma == t->dma) && (g->texes[i].offset == t->offset) && (g->texes[i].size == t->size))
            return g->texes[i].base;
    if (!t->size || (t->size > GPU_TEX_ARENA - g->tex_used))
        return UINT32_MAX;
    base = g->tex_used;
    dst  = (uint8_t *) g->b[B_TSTAGE].map + base;
    for (uint32_t done = 0; done < t->size;) {
        uint32_t off   = t->offset + done;
        uint32_t chunk = 0x1000 - (off & 0xfff);
        uint32_t addr;
        int      target;

        if (chunk > t->size - done)
            chunk = t->size - done;
        if (nv4_dma_translate(nv4, t->dma, off & ~0xfffu, 0, &addr, &target) != NV4_DMA_OK)
            memset(dst + done, 0, chunk);
        else if (!target) {
            for (uint32_t i = 0; i < chunk; i++)
                dst[done + i] = nv4->svga.vram[(addr + (off & 0xfff) + i) & nv4->vram_mask];
        } else
            dma_bm_read(addr + (off & 0xfff), dst + done, chunk, 1);
        done += chunk;
    }
    g->tex_used = (base + t->size + 3) & ~3u;
    g->texes[g->ntex].dma    = t->dma;
    g->texes[g->ntex].offset = t->offset;
    g->texes[g->ntex].size   = t->size;
    g->texes[g->ntex].base   = base;
    g->ntex++;
    return base;
}

static void
gpu_pack_tex(nv4_gpu_tex_t *o, const nv4_3d_tex_t *t, uint32_t base)
{
    o->on       = t->on;
    o->fmt      = t->fmt;
    o->bpp      = t->bpp;
    o->wlog     = t->wlog;
    o->hlog     = t->hlog;
    o->levels   = t->levels;
    o->wrap_u   = t->wrap_u;
    o->wrap_v   = t->wrap_v;
    o->min      = t->min;
    o->mag      = t->mag;
    o->lod_bias = t->lod_bias;
    o->center   = t->center;
    o->ckey     = t->ckey;
    o->base     = base;
    for (int l = 0; l < 12; l++)
        o->level_off[l] = t->level_off[l];
}

static void
gpu_pack_state(nv4_gpu_state_t *o, const nv4_3d_state_t *s)
{
    memset(o, 0, sizeof(*o));
    o->col_base  = s->col_base;
    o->z_base    = s->z_base;
    o->col_pitch = s->col_pitch;
    o->z_pitch   = s->z_pitch;
    o->col_lim   = s->col_lim;
    o->z_lim     = s->z_lim;
    o->col_fmt   = s->col_fmt;
    o->bpp       = s->bpp;
    o->z_on      = s->z_on;
    o->dx6       = s->dx6;
    o->origin    = s->origin;
    o->color_key = s->color_key;
    o->texblend  = s->texblend;
    o->comb_a0   = s->comb_a[0];
    o->comb_a1   = s->comb_a[1];
    o->comb_c0   = s->comb_c[0];
    o->comb_c1   = s->comb_c[1];
    o->factor    = s->factor;
    o->flat      = s->flat;
    o->persp     = s->persp;
    o->spec      = s->spec;
    o->fog       = s->fog;
    o->ablend    = s->ablend;
    o->sblend    = s->sblend;
    o->dblend    = s->dblend;
    o->fog_color = s->fog_color;
    o->atest     = s->atest;
    o->afunc     = s->afunc;
    o->aref      = s->aref;
    o->zen       = s->zen;
    o->zfunc     = s->zfunc;
    o->zwrite    = s->zwrite;
    o->zfloat    = s->zfloat;
    o->dither    = s->dither;
    o->wr_a      = s->wr_a;
    o->wr_r      = s->wr_r;
    o->wr_g      = s->wr_g;
    o->wr_b      = s->wr_b;
    o->sten      = s->sten;
    o->sfunc     = s->sfunc;
    o->sref      = s->sref;
    o->smask_r   = s->smask_r;
    o->smask_w   = s->smask_w;
    o->sop_fail  = s->sop_fail;
    o->sop_zfail = s->sop_zfail;
    o->sop_zpass = s->sop_zpass;
    o->swrite    = s->swrite;
}

static void
gpu_barrier(nv4_gpu_t *g, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

    mb.srcAccessMask = sa;
    mb.dstAccessMask = da;
    g->vkCmdPipelineBarrier(g->cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

int
nv4_gpu_shade(struct nv4_gpu_s *g, nv4_t *nv4, const nv4_3d_batch_t *b)
{
    nv4_gpu_state_t *gs    = (nv4_gpu_state_t *) g->b[B_STATES].map;
    nv4_gpu_span_t  *gsp   = (nv4_gpu_span_t *) g->b[B_SPANS].map;
    nv4_gpu_row_t   *gr    = (nv4_gpu_row_t *) g->b[B_ROWS].map;
    uint8_t         *stage = (uint8_t *) g->b[B_VSTAGE].map;
    VkBufferCopy     up[3], down[2];
    int              nup = 0, ndown = 0;
    uint32_t         nrows = 0;
    uint32_t         pc[2];
    int              zrange;

    if (!b->wr_valid || (b->wr_c_hi >= g->vram_size) || (b->wr_c_lo > b->wr_c_hi))
        goto fallback;
    zrange = b->wr_z_lo <= b->wr_z_hi;
    if (zrange && (b->wr_z_hi >= g->vram_size))
        goto fallback;

    /* draw states and their textures */
    g->ntex     = 0;
    g->tex_used = 0;
    for (int i = 0; i < b->nstates; i++) {
        const nv4_3d_state_t *s = &b->states[i];

        gpu_pack_state(&gs[i], s);
        for (int n = 0; n < 2; n++) {
            uint32_t base = 0;

            if (s->tex[n].on && ((base = gpu_gather_tex(g, nv4, &s->tex[n])) == UINT32_MAX))
                goto fallback;
            gpu_pack_tex(&gs[i].tex[n], &s->tex[n], base);
        }
    }
    memcpy(g->b[B_TRIS].map, b->tris, sizeof(nv4_3d_tri_t) * b->ntris);

    /* spans grouped by row, each row in submission order */
    memset(g->row_count, 0, sizeof(g->row_count));
    for (int i = 0; i < b->nspans; i++)
        g->row_count[b->spans[i].y & 0xfff]++;
    {
        uint32_t at = 0;

        for (int y = 0; y < 4096; y++) {
            g->row_first[y] = at;
            if (g->row_count[y]) {
                gr[nrows].first = at;
                gr[nrows].count = g->row_count[y];
                nrows++;
            }
            at += g->row_count[y];
        }
    }
    for (int i = 0; i < b->nspans; i++) {
        const nv4_3d_span_t *sp = &b->spans[i];
        nv4_gpu_span_t      *o  = &gsp[g->row_first[sp->y & 0xfff]++];

        o->y   = sp->y;
        o->x0  = sp->x0;
        o->x1  = sp->x1;
        o->tri = sp->tri;
    }
    if (!nrows)
        goto fallback;

    /* rows in */
    up[nup].srcOffset = up[nup].dstOffset = b->wr_c_lo;
    up[nup].size                          = b->wr_c_hi - b->wr_c_lo + 1;
    memcpy(stage + b->wr_c_lo, nv4->svga.vram + b->wr_c_lo, up[nup].size);
    down[ndown++] = up[nup++];
    if (zrange) {
        up[nup].srcOffset = up[nup].dstOffset = b->wr_z_lo;
        up[nup].size                          = b->wr_z_hi - b->wr_z_lo + 1;
        memcpy(stage + b->wr_z_lo, nv4->svga.vram + b->wr_z_lo, up[nup].size);
        down[ndown++] = up[nup++];
    }

    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo             si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };

        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g->vkResetCommandBuffer(g->cb, 0);
        g->vkBeginCommandBuffer(g->cb, &bi);
        g->vkCmdCopyBuffer(g->cb, g->b[B_VSTAGE].buf, g->b[B_VRAM].buf, nup, up);
        if (g->tex_used) {
            VkBufferCopy tc = { 0, 0, g->tex_used };

            g->vkCmdCopyBuffer(g->cb, g->b[B_TSTAGE].buf, g->b[B_TEX].buf, 1, &tc);
        }
        gpu_barrier(g, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        g->vkCmdBindPipeline(g->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipe);
        g->vkCmdBindDescriptorSets(g->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pl, 0, 1, &g->dset, 0, NULL);
        pc[0] = nrows;
        pc[1] = nv4->vram_mask;
        g->vkCmdPushConstants(g->cb, g->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        g->vkCmdDispatch(g->cb, nrows, 1, 1);
        gpu_barrier(g, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
        g->vkCmdCopyBuffer(g->cb, g->b[B_VRAM].buf, g->b[B_VSTAGE].buf, ndown, down);
        gpu_barrier(g, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_ACCESS_HOST_READ_BIT);
        g->vkEndCommandBuffer(g->cb);
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &g->cb;
        g->vkResetFences(g->dev, 1, &g->fence);
        if (g->vkQueueSubmit(g->queue, 1, &si, g->fence) != VK_SUCCESS)
            goto fallback;
        if (g->vkWaitForFences(g->dev, 1, &g->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            goto fallback;
    }

    /* rows out */
    for (int i = 0; i < ndown; i++)
        memcpy(nv4->svga.vram + down[i].srcOffset, stage + down[i].srcOffset, down[i].size);
    for (uint32_t a = b->wr_c_lo & ~0xfffu; a <= b->wr_c_hi; a += 0x1000)
        nv4->svga.changedvram[a >> 12] = nv4->svga.monitor->mon_changeframecount;
    g->batches++;
    return 1;

fallback:
    g->fallbacks++;
    return 0;
}

#endif
