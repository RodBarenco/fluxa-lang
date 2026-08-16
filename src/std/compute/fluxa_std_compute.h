#ifndef FLUXA_STD_COMPUTE_H
#define FLUXA_STD_COMPUTE_H

/* fluxa_std_compute.h — general-purpose GPU computation.
 *
 * No window, no swapchain, no input, no notion of a frame. std.compute treats
 * the GPU as a calculating machine: buffers, kernels, dispatch, and a way to
 * ask whether the work is done.
 *
 *   compute.init()                                → dyn    context
 *   compute.create_buffer(ctx, size, usage)       → int    handle
 *   compute.upload(ctx, buf, arr, offset)         → nil
 *   compute.load_kernel(ctx, "calc.spv")          → int    handle
 *   compute.begin / bind_kernel / bind_buffer / dispatch / end
 *   compute.wait(ctx, ticket)  /  compute.ready(ctx, ticket)
 *   compute.download(ctx, buf, offset, size)      → dyn
 *
 * Handles and lifetime
 * --------------------
 * The context is a dyn cursor and is meant to be held in `prst dyn`, so it
 * survives a hot reload and the GPU work of the previous run is not thrown
 * away on every save. Buffers and kernels are plain ints, which is what lets a
 * Block method receive them.
 *
 * That combination has a trap, and it is the same one that produces "Address
 * already in use" for sockets. An in-process reload keeps the context alive, so
 * an int handle stays valid — good. But a runtime swap (handover, update)
 * cannot carry a pointer across the snapshot: the context is rebuilt from its
 * declaration while a `prst int` handle is restored intact, leaving a number
 * that refers to a resource table which no longer exists.
 *
 * So a handle is not a bare index. It carries the generation of the context
 * that issued it:
 *
 *     handle = (generation << 20) | (slot + 1)
 *
 * Every compute.init() takes the next generation. A handle from a previous
 * context therefore fails a check instead of silently addressing whatever now
 * occupies that slot — which would be a wrong answer rather than an error, and
 * far harder to find. The message says so explicitly.
 *
 * Backends
 * --------
 * Real: Vulkan (compute queue only — no surface, no swapchain, so it runs
 * headless on a server as happily as on a desktop).
 *
 * Stub: the full lifecycle in host memory. Buffers are real allocations,
 * upload/download/copy/fill genuinely move bytes, and every argument check and
 * handle rule behaves identically. What the stub cannot do is run a kernel:
 * dispatch is a no-op, because executing SPIR-V without a GPU is not something
 * a stub can pretend at. That line is deliberate — the lifecycle is testable
 * everywhere, and compute.version() states plainly which backend is present so
 * a program is never fooled into thinking a kernel ran.
 *
 * Data
 * ----
 * upload takes an `int arr` or a `float arr`. Fluxa arrays are arrays of Value,
 * not packed bytes, so the conversion to a contiguous 32-bit block happens
 * here: an int array becomes int32, a float array becomes float32 — the two
 * types a compute shader actually reads.
 *
 * All functions that touch the device must run inside a danger {} block.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../scope.h"
#include "../../err.h"

#ifdef FLUXA_COMPUTE_VULKAN
#include <vulkan/vulkan.h>
#endif

/* ── Caps ────────────────────────────────────────────────────────────
 * Each bounds a decision made from user input. Generous for real work, and
 * still enough to keep a typo from asking for an absurd allocation. */
#define CMP_MAX_BUFFER_BYTES  (1024u*1024u*1024u)   /* 1 GiB per buffer     */
#define CMP_MAX_BUFFERS       4096
#define CMP_MAX_KERNELS       512
#define CMP_MAX_SPV_BYTES     (64u*1024u*1024u)     /* SPIR-V module size   */
#define CMP_MAX_PUSH_BYTES    128                   /* Vulkan guarantees 128 */
#define CMP_MAX_SLOT          31
#define CMP_MAGIC             0x434D5055u           /* 'CMPU'               */
#define CMP_SPV_MAGIC         0x07230203u

/* Handle layout: generation in the high bits, slot+1 in the low 20. Slot 0 is
 * never used so that 0 is always an invalid handle — the same convention the
 * language uses for socket and request handles. */
#define CMP_SLOT_BITS   20
#define CMP_SLOT_MASK   ((1u << CMP_SLOT_BITS) - 1u)
#define CMP_MAKE_HANDLE(gen,slot)  (long)(((unsigned long)(gen) << CMP_SLOT_BITS) \
                                          | (unsigned long)((slot) + 1))
#define CMP_HANDLE_GEN(h)   (unsigned)(((unsigned long)(h)) >> CMP_SLOT_BITS)
#define CMP_HANDLE_SLOT(h)  (int)((((unsigned long)(h)) & CMP_SLOT_MASK) - 1u)

typedef struct {
    unsigned char *host;      /* stub storage; staging mirror under Vulkan   */
    size_t         size;
    int            alive;
    char           usage[16];
#ifdef FLUXA_COMPUTE_VULKAN
    VkBuffer       buf;
    VkDeviceMemory mem;
#endif
} CmpBuffer;

typedef struct {
    char *path;
    char *entry;
    int   alive;
#ifdef FLUXA_COMPUTE_VULKAN
    VkShaderModule       module;
    VkDescriptorSetLayout dset_layout;
    VkPipelineLayout     pipe_layout;
    VkPipeline           pipeline;
#endif
} CmpKernel;

#define CMP_MAX_RECORDED 4096

/* One recorded step of a batch: a dispatch with the bindings in force when it
 * was issued, or a barrier holding its place in the sequence. */
typedef struct {
    long     kernel;                  /* 0 when this entry is a barrier */
    int      barrier;
    unsigned x, y, z;
    long     slots[CMP_MAX_SLOT + 1];
} CmpRec;

typedef struct {
    unsigned   magic;
    unsigned   generation;

    CmpBuffer *bufs;    int nbufs;
    CmpKernel *kerns;   int nkerns;

    long  bound_kernel;             /* handle, 0 = none                     */
    long  bound_slots[CMP_MAX_SLOT + 1];
    int   recording;                /* inside begin/end                     */
    int   dispatches;               /* dispatches in the current batch      */
    long  next_ticket;
    long  last_ticket;
    long  completed_ticket;         /* everything <= this has finished      */

    char  profile_name[64];
    int   profiling;
    double profile_last_ms;

    unsigned char push_data[CMP_MAX_PUSH_BYTES];
    int           push_size;

    CmpRec *rec;      int nrec;

#ifdef FLUXA_COMPUTE_VULKAN
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         qfamily;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;
    VkDescriptorPool dpool;
    char             device_name[256];
    uint32_t         max_wg[3];
    VkDeviceSize     max_alloc;
    VkDeviceSize     mem_total;
    /* Every binding of the descriptor layout must be written or validation
     * rejects the set, so slots the script never bound point here. One word,
     * shared, and inert. */
    VkBuffer         dummy_buf;
    VkDeviceMemory   dummy_mem;
#endif
} CmpCtx;

/* Generation counter, shared by every context created in this process. A
 * context that is torn down and rebuilt — which is exactly what a runtime swap
 * does — never reuses the generation of the one before it. */
static unsigned g_cmp_generation = 0;

#ifdef FLUXA_COMPUTE_VULKAN
/* ── Vulkan backend ──────────────────────────────────────────────────
 * Compute only: no surface, no swapchain, no presentation. That is what lets
 * the same code run on a headless server and on a desktop, and it keeps the
 * device selection simple — any physical device with a queue family that
 * supports VK_QUEUE_COMPUTE_BIT will do.
 *
 * Buffers are allocated HOST_VISIBLE | HOST_COHERENT and mapped for the life
 * of the buffer. That trades some device-local bandwidth for a great deal of
 * simplicity: upload and download become memcpy, with no staging buffer and no
 * transfer command to synchronise. For the workloads this library is aimed at
 * — data that comes from the program, gets computed on, and goes back — it is
 * the right trade. A device-local path with staging can be added later without
 * changing a single line of the Fluxa-facing API.
 */

static int cmp_vk_find_mem(CmpCtx *c, uint32_t type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    }
    return -1;
}

static int cmp_vk_init(CmpCtx *c, char *eb, size_t ebn) {
    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "fluxa";
    app.applicationVersion = 1;
    app.pEngineName        = "fluxa";
    app.engineVersion      = 1;
    app.apiVersion         = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &c->instance) != VK_SUCCESS) {
        snprintf(eb, ebn, "init: no Vulkan instance — is a driver (ICD) installed?");
        return 0;
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c->instance, &n, NULL);
    if (n == 0) {
        vkDestroyInstance(c->instance, NULL); c->instance = VK_NULL_HANDLE;
        snprintf(eb, ebn, "init: no Vulkan device found");
        return 0;
    }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    vkEnumeratePhysicalDevices(c->instance, &n, devs);

    /* Prefer a discrete GPU when there is one, but accept anything that can
     * compute — a software rasteriser is a perfectly good target for tests. */
    int chosen = -1, chosen_q = -1, chosen_discrete = 0;
    for (uint32_t d = 0; d < n; d++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qn, NULL);
        if (qn == 0 || qn > 32) continue;
        VkQueueFamilyProperties qs[32];
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &qn, qs);
        for (uint32_t q = 0; q < qn; q++) {
            if (!(qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(devs[d], &pp);
            int discrete = (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
            if (chosen < 0 || (discrete && !chosen_discrete)) {
                chosen = (int)d; chosen_q = (int)q; chosen_discrete = discrete;
            }
            break;
        }
    }
    if (chosen < 0) {
        vkDestroyInstance(c->instance, NULL); c->instance = VK_NULL_HANDLE;
        snprintf(eb, ebn, "init: no device with a compute queue");
        return 0;
    }
    c->phys    = devs[chosen];
    c->qfamily = (uint32_t)chosen_q;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(c->phys, &props);
    snprintf(c->device_name, sizeof(c->device_name), "%s", props.deviceName);
    c->max_wg[0] = props.limits.maxComputeWorkGroupCount[0];
    c->max_wg[1] = props.limits.maxComputeWorkGroupCount[1];
    c->max_wg[2] = props.limits.maxComputeWorkGroupCount[2];
    c->max_alloc = props.limits.maxStorageBufferRange;

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c->phys, &mp);
    c->mem_total = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            c->mem_total += mp.memoryHeaps[i].size;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = c->qfamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;
    if (vkCreateDevice(c->phys, &dci, NULL, &c->device) != VK_SUCCESS) {
        vkDestroyInstance(c->instance, NULL); c->instance = VK_NULL_HANDLE;
        snprintf(eb, ebn, "init: could not create the logical device");
        return 0;
    }
    vkGetDeviceQueue(c->device, c->qfamily, 0, &c->queue);

    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c->qfamily;
    if (vkCreateCommandPool(c->device, &pci, NULL, &c->pool) != VK_SUCCESS) {
        vkDestroyDevice(c->device, NULL); c->device = VK_NULL_HANDLE;
        vkDestroyInstance(c->instance, NULL); c->instance = VK_NULL_HANDLE;
        snprintf(eb, ebn, "init: could not create the command pool");
        return 0;
    }

    VkCommandBufferAllocateInfo cbi;
    memset(&cbi, 0, sizeof(cbi));
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = c->pool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(c->device, &cbi, &c->cmd);

    VkFenceCreateInfo fci;
    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    /* Created already signalled. cmp_vk_submit waits on this fence before
     * reusing the command buffer, so an unsignalled fence would deadlock on
     * the very first batch — waiting forever for a submission that has not
     * happened yet. Starting signalled makes the first wait a no-op and every
     * wait after it mean what it should. */
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(c->device, &fci, NULL, &c->fence);

    VkDescriptorPoolSize psz;
    psz.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    psz.descriptorCount = (CMP_MAX_SLOT + 1) * 64;
    VkDescriptorPoolCreateInfo dpi;
    memset(&dpi, 0, sizeof(dpi));
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets       = 64;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes    = &psz;
    vkCreateDescriptorPool(c->device, &dpi, NULL, &c->dpool);

    /* One-word buffer for descriptor bindings the script left unbound. */
    {
        VkBufferCreateInfo dbi;
        memset(&dbi, 0, sizeof(dbi));
        dbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        dbi.size  = 4;
        dbi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        dbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(c->device, &dbi, NULL, &c->dummy_buf) == VK_SUCCESS) {
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(c->device, c->dummy_buf, &mr);
            int mt = cmp_vk_find_mem(c, mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (mt >= 0) {
                VkMemoryAllocateInfo dmi;
                memset(&dmi, 0, sizeof(dmi));
                dmi.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                dmi.allocationSize  = mr.size;
                dmi.memoryTypeIndex = (uint32_t)mt;
                if (vkAllocateMemory(c->device, &dmi, NULL, &c->dummy_mem) == VK_SUCCESS)
                    vkBindBufferMemory(c->device, c->dummy_buf, c->dummy_mem, 0);
            }
        }
    }
    return 1;
}

static void cmp_vk_close(CmpCtx *c) {
    if (c->device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(c->device);
    for (int i = 0; i < c->nkerns; i++) {
        CmpKernel *k = &c->kerns[i];
        if (!k->alive) continue;
        if (k->pipeline)    vkDestroyPipeline(c->device, k->pipeline, NULL);
        if (k->pipe_layout) vkDestroyPipelineLayout(c->device, k->pipe_layout, NULL);
        if (k->dset_layout) vkDestroyDescriptorSetLayout(c->device, k->dset_layout, NULL);
        if (k->module)      vkDestroyShaderModule(c->device, k->module, NULL);
    }
    for (int i = 0; i < c->nbufs; i++) {
        CmpBuffer *b = &c->bufs[i];
        if (!b->alive) continue;
        if (b->buf) vkDestroyBuffer(c->device, b->buf, NULL);
        if (b->mem) vkFreeMemory(c->device, b->mem, NULL);
    }
    if (c->dummy_buf) vkDestroyBuffer(c->device, c->dummy_buf, NULL);
    if (c->dummy_mem) vkFreeMemory(c->device, c->dummy_mem, NULL);
    if (c->dpool) vkDestroyDescriptorPool(c->device, c->dpool, NULL);
    if (c->fence) vkDestroyFence(c->device, c->fence, NULL);
    if (c->pool)  vkDestroyCommandPool(c->device, c->pool, NULL);
    vkDestroyDevice(c->device, NULL);
    if (c->instance) vkDestroyInstance(c->instance, NULL);
    c->device = VK_NULL_HANDLE; c->instance = VK_NULL_HANDLE;
}

static int cmp_vk_has(CmpCtx *c, const char *feat) {
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(c->phys, &f);
    if (!strcmp(feat,"float64"))    return f.shaderFloat64 ? 1 : 0;
    if (!strcmp(feat,"int64"))      return f.shaderInt64   ? 1 : 0;
    if (!strcmp(feat,"int16"))      return f.shaderInt16   ? 1 : 0;
    if (!strcmp(feat,"float16"))    return 0;   /* needs an extension query */
    if (!strcmp(feat,"timestamps")) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(c->phys, &p);
        return p.limits.timestampComputeAndGraphics ? 1 : 0;
    }
    return 0;
}

static int cmp_vk_make_buffer(CmpCtx *c, CmpBuffer *b, char *eb, size_t ebn) {
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = (VkDeviceSize)b->size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!strcmp(b->usage,"storage"))  bci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!strcmp(b->usage,"uniform"))  bci.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (!strcmp(b->usage,"indirect")) bci.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(c->device, &bci, NULL, &b->buf) != VK_SUCCESS) {
        snprintf(eb, ebn, "create_buffer: the device refused the allocation");
        return 0;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(c->device, b->buf, &mr);
    int mt = cmp_vk_find_mem(c, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        vkDestroyBuffer(c->device, b->buf, NULL); b->buf = VK_NULL_HANDLE;
        snprintf(eb, ebn, "create_buffer: no host-visible memory type available");
        return 0;
    }
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(c->device, &mai, NULL, &b->mem) != VK_SUCCESS) {
        vkDestroyBuffer(c->device, b->buf, NULL); b->buf = VK_NULL_HANDLE;
        snprintf(eb, ebn, "create_buffer: out of device memory");
        return 0;
    }
    vkBindBufferMemory(c->device, b->buf, b->mem, 0);
    return 1;
}

static void cmp_vk_free_buffer(CmpCtx *c, CmpBuffer *b) {
    if (b->buf) { vkDestroyBuffer(c->device, b->buf, NULL); b->buf = VK_NULL_HANDLE; }
    if (b->mem) { vkFreeMemory(c->device, b->mem, NULL);    b->mem = VK_NULL_HANDLE; }
}

/* Host memory is the source of truth in this backend; these push it to the
 * device and pull it back. With HOST_COHERENT memory no explicit flush is
 * needed, so both are a map, a memcpy and an unmap. */
static int cmp_vk_upload(CmpCtx *c, CmpBuffer *b, size_t off, size_t n,
                          char *eb, size_t ebn) {
    void *p = NULL;
    if (vkMapMemory(c->device, b->mem, off, n, 0, &p) != VK_SUCCESS) {
        snprintf(eb, ebn, "upload: could not map device memory");
        return 0;
    }
    memcpy(p, b->host + off, n);
    vkUnmapMemory(c->device, b->mem);
    return 1;
}

static int cmp_vk_download(CmpCtx *c, CmpBuffer *b, size_t off, size_t n,
                            char *eb, size_t ebn) {
    void *p = NULL;
    if (vkMapMemory(c->device, b->mem, off, n, 0, &p) != VK_SUCCESS) {
        snprintf(eb, ebn, "download: could not map device memory");
        return 0;
    }
    memcpy(b->host + off, p, n);
    vkUnmapMemory(c->device, b->mem);
    return 1;
}

static int cmp_vk_copy(CmpCtx *c, CmpBuffer *s, CmpBuffer *d,
                        size_t soff, size_t doff, size_t n,
                        char *eb, size_t ebn) {
    /* The host mirror was already updated by the caller; push the destination
     * region so the device sees the same bytes. */
    (void)s; (void)soff;
    return cmp_vk_upload(c, d, doff, n, eb, ebn);
}

static int cmp_vk_make_kernel(CmpCtx *c, CmpKernel *k,
                               const unsigned char *spv, size_t n,
                               char *eb, size_t ebn) {
    VkShaderModuleCreateInfo smi;
    memset(&smi, 0, sizeof(smi));
    smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = n;
    smi.pCode    = (const uint32_t *)(const void *)spv;
    if (vkCreateShaderModule(c->device, &smi, NULL, &k->module) != VK_SUCCESS) {
        snprintf(eb, ebn, "load_kernel: the driver rejected the SPIR-V module");
        return 0;
    }

    /* One descriptor set with CMP_MAX_SLOT+1 storage-buffer bindings, so a
     * shader can use any slot the API allows without the pipeline layout
     * having to be described from Fluxa. */
    VkDescriptorSetLayoutBinding binds[CMP_MAX_SLOT + 1];
    for (int i = 0; i <= CMP_MAX_SLOT; i++) {
        memset(&binds[i], 0, sizeof(binds[i]));
        binds[i].binding         = (uint32_t)i;
        binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli;
    memset(&dli, 0, sizeof(dli));
    dli.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = CMP_MAX_SLOT + 1;
    dli.pBindings    = binds;
    if (vkCreateDescriptorSetLayout(c->device, &dli, NULL, &k->dset_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(c->device, k->module, NULL); k->module = VK_NULL_HANDLE;
        snprintf(eb, ebn, "load_kernel: could not create the descriptor layout");
        return 0;
    }

    VkPushConstantRange pcr;
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset     = 0;
    pcr.size       = CMP_MAX_PUSH_BYTES;
    VkPipelineLayoutCreateInfo pli;
    memset(&pli, 0, sizeof(pli));
    pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount         = 1;
    pli.pSetLayouts            = &k->dset_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(c->device, &pli, NULL, &k->pipe_layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(c->device, k->dset_layout, NULL);
        vkDestroyShaderModule(c->device, k->module, NULL);
        k->dset_layout = VK_NULL_HANDLE; k->module = VK_NULL_HANDLE;
        snprintf(eb, ebn, "load_kernel: could not create the pipeline layout");
        return 0;
    }

    VkComputePipelineCreateInfo cpi;
    memset(&cpi, 0, sizeof(cpi));
    cpi.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = k->module;
    cpi.stage.pName  = k->entry;
    cpi.layout       = k->pipe_layout;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &cpi, NULL,
                                 &k->pipeline) != VK_SUCCESS) {
        vkDestroyPipelineLayout(c->device, k->pipe_layout, NULL);
        vkDestroyDescriptorSetLayout(c->device, k->dset_layout, NULL);
        vkDestroyShaderModule(c->device, k->module, NULL);
        k->pipe_layout = VK_NULL_HANDLE; k->dset_layout = VK_NULL_HANDLE;
        k->module = VK_NULL_HANDLE;
        snprintf(eb, ebn, "load_kernel: pipeline creation failed — check that "
                          "the entry point name matches the shader");
        return 0;
    }
    return 1;
}

static void cmp_vk_free_kernel(CmpCtx *c, CmpKernel *k) {
    vkDeviceWaitIdle(c->device);
    if (k->pipeline)    { vkDestroyPipeline(c->device, k->pipeline, NULL); k->pipeline = VK_NULL_HANDLE; }
    if (k->pipe_layout) { vkDestroyPipelineLayout(c->device, k->pipe_layout, NULL); k->pipe_layout = VK_NULL_HANDLE; }
    if (k->dset_layout) { vkDestroyDescriptorSetLayout(c->device, k->dset_layout, NULL); k->dset_layout = VK_NULL_HANDLE; }
    if (k->module)      { vkDestroyShaderModule(c->device, k->module, NULL); k->module = VK_NULL_HANDLE; }
}

/* Command recording.
 *
 * A batch is recorded lazily rather than as the script calls dispatch. The
 * reason is the descriptor set: it has to name the buffers bound at the moment
 * of the dispatch, and Fluxa binds them one call at a time. Recording at
 * compute.end() means the bindings are known and settled, and it keeps the
 * whole batch in one command buffer with one submit and one fence.
 *
 * The batch is therefore remembered as a small list of dispatches, each with
 * the kernel and the slot bindings in force when it was issued. */
static void cmp_vk_dispatch(CmpCtx *c, uint32_t x, uint32_t y, uint32_t z) {
    if (c->nrec >= CMP_MAX_RECORDED) return;   /* checked by the caller */
    CmpRec *r = &c->rec[c->nrec++];
    r->kernel = c->bound_kernel;
    r->x = x; r->y = y; r->z = z;
    memcpy(r->slots, c->bound_slots, sizeof(r->slots));
}

static void cmp_vk_barrier(CmpCtx *c, const char *kind) {
    if (c->nrec >= CMP_MAX_RECORDED) return;
    /* A barrier is recorded as an entry with no kernel, so it keeps its
     * position in the sequence relative to the dispatches around it. */
    CmpRec *r = &c->rec[c->nrec++];
    memset(r, 0, sizeof(*r));
    r->kernel  = 0;
    r->barrier = 1;
    (void)kind;   /* every kind maps to the same shader-write/read barrier */
}

static int cmp_vk_submit(CmpCtx *c, char *eb, size_t ebn) {
    if (c->nrec == 0) { c->nrec = 0; return 1; }   /* empty batch: nothing to do */

    /* The previous submission must have finished before its command buffer is
     * reset — the fence is what guarantees that. */
    vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(c->device, 1, &c->fence);
    vkResetCommandBuffer(c->cmd, 0);

    /* Descriptor sets from the previous batch are no longer referenced. */
    vkResetDescriptorPool(c->device, c->dpool, 0);

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(c->cmd, &bi) != VK_SUCCESS) {
        c->nrec = 0;
        snprintf(eb, ebn, "end: could not begin recording");
        return 0;
    }

    for (int i = 0; i < c->nrec; i++) {
        CmpRec *r = &c->rec[i];

        if (r->barrier) {
            VkMemoryBarrier mb;
            memset(&mb, 0, sizeof(mb));
            mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT  | VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(c->cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &mb, 0, NULL, 0, NULL);
            continue;
        }

        int kslot = CMP_HANDLE_SLOT(r->kernel);
        if (kslot < 0 || kslot >= c->nkerns || !c->kerns[kslot].alive) continue;
        CmpKernel *k = &c->kerns[kslot];

        VkDescriptorSetAllocateInfo dai;
        memset(&dai, 0, sizeof(dai));
        dai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool     = c->dpool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts        = &k->dset_layout;
        VkDescriptorSet dset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(c->device, &dai, &dset) != VK_SUCCESS) {
            vkEndCommandBuffer(c->cmd);
            c->nrec = 0;
            snprintf(eb, ebn, "end: ran out of descriptor sets in this batch");
            return 0;
        }

        /* Every binding in the layout must be written, or validation rejects
         * the set. Slots the script did not bind point at a one-word dummy
         * buffer, so an unbound slot is inert rather than undefined. */
        VkDescriptorBufferInfo binfo[CMP_MAX_SLOT + 1];
        VkWriteDescriptorSet   wr[CMP_MAX_SLOT + 1];
        int nw = 0;
        for (int s = 0; s <= CMP_MAX_SLOT; s++) {
            VkBuffer vb = c->dummy_buf;
            VkDeviceSize sz = 4;
            long h = r->slots[s];
            if (h > 0 && CMP_HANDLE_GEN(h) == c->generation) {
                int bslot = CMP_HANDLE_SLOT(h);
                if (bslot >= 0 && bslot < c->nbufs && c->bufs[bslot].alive) {
                    vb = c->bufs[bslot].buf;
                    sz = (VkDeviceSize)c->bufs[bslot].size;
                }
            }
            binfo[s].buffer = vb;
            binfo[s].offset = 0;
            binfo[s].range  = sz;

            memset(&wr[nw], 0, sizeof(wr[nw]));
            wr[nw].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[nw].dstSet          = dset;
            wr[nw].dstBinding      = (uint32_t)s;
            wr[nw].descriptorCount = 1;
            wr[nw].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr[nw].pBufferInfo     = &binfo[s];
            nw++;
        }
        vkUpdateDescriptorSets(c->device, (uint32_t)nw, wr, 0, NULL);

        vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k->pipeline);
        vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                k->pipe_layout, 0, 1, &dset, 0, NULL);
        if (c->push_size > 0)
            vkCmdPushConstants(c->cmd, k->pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, (uint32_t)c->push_size, c->push_data);
        vkCmdDispatch(c->cmd, r->x, r->y, r->z);
    }

    vkEndCommandBuffer(c->cmd);

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &c->cmd;
    c->nrec = 0;
    if (vkQueueSubmit(c->queue, 1, &si, c->fence) != VK_SUCCESS) {
        snprintf(eb, ebn, "end: the queue rejected the batch");
        return 0;
    }
    return 1;
}

static void cmp_vk_wait(CmpCtx *c, long ticket) {
    vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX);
    if (c->completed_ticket < ticket) c->completed_ticket = ticket;
}

/* Non-blocking: asks the fence rather than waiting on it. That is the whole
 * point of a ticket — the CPU keeps working while the GPU does. */
static int cmp_vk_ready(CmpCtx *c, long ticket) {
    if (c->completed_ticket >= ticket) return 1;
    if (vkGetFenceStatus(c->device, c->fence) == VK_SUCCESS) {
        c->completed_ticket = c->last_ticket;
        return c->completed_ticket >= ticket;
    }
    return 0;
}

static double cmp_vk_profile_ms(CmpCtx *c) {
    (void)c;
    return -1.0;
}
#endif /* FLUXA_COMPUTE_VULKAN */


static void cmp_free_ctx(CmpCtx *c) {
    if (!c) return;
    if (c->bufs) {
        for (int i = 0; i < c->nbufs; i++) free(c->bufs[i].host);
        free(c->bufs);
    }
    free(c->rec); c->rec = NULL;
    if (c->kerns) {
        for (int i = 0; i < c->nkerns; i++) {
            free(c->kerns[i].path);
            free(c->kerns[i].entry);
        }
        free(c->kerns);
    }
    c->magic = 0;
    free(c);
}

/* ── Value helpers ───────────────────────────────────────────────────── */
static inline Value cmp_nil(void)    { Value v; v.type=VAL_NIL;   return v; }
static inline Value cmp_int(long n)  { Value v; v.type=VAL_INT;   v.as.integer=n; return v; }
static inline Value cmp_bool(int b)  { Value v; v.type=VAL_BOOL;  v.as.boolean=b?1:0; return v; }
static inline Value cmp_flt(double d){ Value v; v.type=VAL_FLOAT; v.as.real=d; return v; }
static inline Value cmp_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=fxstr_new(s?s:""); return v; }

static inline Value cmp_wrap(CmpCtx *c) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=c;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}

/* ── Dispatch ────────────────────────────────────────────────────────── */
static inline Value fluxa_std_compute_call(const char *fn_name,
                                            const Value *args, int argc,
                                            ErrStack *err, int *had_error,
                                            int line) {
    char errbuf[320];

#define CMP_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"compute.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"compute",line); \
    *had_error=1; return cmp_nil(); } while(0)

#define CMP_NEED(n) do { if(argc<(n)) { \
    snprintf(errbuf,sizeof(errbuf),"compute.%s: expected %d arg(s), got %d",fn_name,(n),argc); \
    errstack_push(err,ERR_FLUXA,errbuf,"compute",line); \
    *had_error=1; return cmp_nil(); } } while(0)

#define CMP_STR(idx,var) \
    if(args[(idx)].type!=VAL_STRING||!args[(idx)].as.string) CMP_ERR("expected str"); \
    const char *(var)=args[(idx)].as.string;

#define CMP_INT(idx,var) \
    if(args[(idx)].type!=VAL_INT) CMP_ERR("expected int"); \
    long (var)=args[(idx)].as.integer;

#define CMP_CTX(idx,var) \
    if(args[(idx)].type!=VAL_DYN||!args[(idx)].as.dyn||args[(idx)].as.dyn->count<1|| \
       args[(idx)].as.dyn->items[0].type!=VAL_PTR||!args[(idx)].as.dyn->items[0].as.ptr) \
        CMP_ERR("invalid context — use compute.init()"); \
    CmpCtx *(var)=(CmpCtx *)args[(idx)].as.dyn->items[0].as.ptr; \
    if((var)->magic!=CMP_MAGIC) CMP_ERR("this context is already closed");

    /* Resolve a buffer handle, rejecting one issued by a different context. */
#define CMP_BUF(ctxv,idx,var) \
    long _h##var = 0; \
    if(args[(idx)].type!=VAL_INT) CMP_ERR("expected an int buffer handle"); \
    _h##var = args[(idx)].as.integer; \
    if(_h##var <= 0) CMP_ERR("invalid buffer handle (0 is never valid)"); \
    if(CMP_HANDLE_GEN(_h##var) != (ctxv)->generation) \
        CMP_ERR("this buffer handle belongs to a previous context — a runtime " \
                "swap rebuilds the context, so handles must be recreated too"); \
    { int _s = CMP_HANDLE_SLOT(_h##var); \
      if(_s < 0 || _s >= (ctxv)->nbufs || !(ctxv)->bufs[_s].alive) \
          CMP_ERR("buffer handle refers to a destroyed or unknown buffer"); } \
    CmpBuffer *(var) = &(ctxv)->bufs[CMP_HANDLE_SLOT(_h##var)];

#define CMP_KERN(ctxv,idx,var) \
    long _k##var = 0; \
    if(args[(idx)].type!=VAL_INT) CMP_ERR("expected an int kernel handle"); \
    _k##var = args[(idx)].as.integer; \
    if(_k##var <= 0) CMP_ERR("invalid kernel handle (0 is never valid)"); \
    if(CMP_HANDLE_GEN(_k##var) != (ctxv)->generation) \
        CMP_ERR("this kernel handle belongs to a previous context — a runtime " \
                "swap rebuilds the context, so handles must be recreated too"); \
    { int _s = CMP_HANDLE_SLOT(_k##var); \
      if(_s < 0 || _s >= (ctxv)->nkerns || !(ctxv)->kerns[_s].alive) \
          CMP_ERR("kernel handle refers to a destroyed or unknown kernel"); } \
    CmpKernel *(var) = &(ctxv)->kerns[CMP_HANDLE_SLOT(_k##var)];

    if (!strcmp(fn_name,"version")) {
#ifdef FLUXA_COMPUTE_VULKAN
        return cmp_str("vulkan compute");
#else
        return cmp_str("stub backend — lifecycle only, kernels do not run "
                       "(build with FLUXA_COMPUTE_VULKAN=1 for a real device)");
#endif
    }

    /* ── compute.init() → dyn ─────────────────────────────────────── */
    if (!strcmp(fn_name,"init")) {
        CmpCtx *c = (CmpCtx *)calloc(1, sizeof(CmpCtx));
        if (!c) CMP_ERR("init: out of memory");
        c->magic       = CMP_MAGIC;
        c->generation  = ++g_cmp_generation;
        c->next_ticket = 1;
        c->bufs  = (CmpBuffer *)calloc(CMP_MAX_BUFFERS, sizeof(CmpBuffer));
        c->kerns = (CmpKernel *)calloc(CMP_MAX_KERNELS, sizeof(CmpKernel));
        c->rec   = (CmpRec *)calloc(CMP_MAX_RECORDED, sizeof(CmpRec));
        if (!c->bufs || !c->kerns || !c->rec) { cmp_free_ctx(c); CMP_ERR("init: out of memory"); }
        c->nbufs  = CMP_MAX_BUFFERS;
        c->nkerns = CMP_MAX_KERNELS;
        for (int i = 0; i < CMP_MAX_BUFFERS; i++) c->bufs[i].alive = 0;
        for (int i = 0; i < CMP_MAX_KERNELS; i++) c->kerns[i].alive = 0;
#ifdef FLUXA_COMPUTE_VULKAN
        if (!cmp_vk_init(c, errbuf, sizeof(errbuf))) {
            cmp_free_ctx(c);
            errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
            *had_error=1; return cmp_nil();
        }
#endif
        return cmp_wrap(c);
    }

    if (!strcmp(fn_name,"close")) {
        CMP_NEED(1);
        /* Releasing is silent on an already-released context, matching
         * pg.free_result, json2.discard and image.discard. */
        if (args[0].type!=VAL_DYN || !args[0].as.dyn || args[0].as.dyn->count<1 ||
            args[0].as.dyn->items[0].type!=VAL_PTR || !args[0].as.dyn->items[0].as.ptr)
            return cmp_nil();
        CmpCtx *c = (CmpCtx *)args[0].as.dyn->items[0].as.ptr;
        if (c->magic != CMP_MAGIC) return cmp_nil();
#ifdef FLUXA_COMPUTE_VULKAN
        cmp_vk_close(c);
#endif
        cmp_free_ctx(c);
        args[0].as.dyn->items[0].as.ptr = NULL;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"wait_idle")) {
        CMP_NEED(1); CMP_CTX(0,c);
#ifdef FLUXA_COMPUTE_VULKAN
        vkDeviceWaitIdle(c->device);
#endif
        c->completed_ticket = c->last_ticket;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"device_name")) {
        CMP_NEED(1); CMP_CTX(0,c);
#ifdef FLUXA_COMPUTE_VULKAN
        return cmp_str(c->device_name);
#else
        (void)c;
        return cmp_str("stub device (no GPU)");
#endif
    }

    /* compute.has(ctx, feature) → bool
     * Feature names are Fluxa's own, not Vulkan's, so a script never has to
     * know an extension string. An unknown name is an error rather than a
     * silent false: false would read as "the GPU lacks it" when the truth is
     * "you asked for something that does not exist". */
    if (!strcmp(fn_name,"has")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_STR(1,feat);
        (void)c;
        int known = (!strcmp(feat,"float64")  || !strcmp(feat,"int64")   ||
                     !strcmp(feat,"int16")    || !strcmp(feat,"float16") ||
                     !strcmp(feat,"timestamps"));
        if (!known) CMP_ERR("has: unknown feature "
            "(float64, int64, int16, float16, timestamps)");
#ifdef FLUXA_COMPUTE_VULKAN
        return cmp_bool(cmp_vk_has(c, feat));
#else
        return cmp_bool(0);
#endif
    }

    /* ── Buffers ──────────────────────────────────────────────────── */

    if (!strcmp(fn_name,"create_buffer")) {
        CMP_NEED(3); CMP_CTX(0,c); CMP_INT(1,size); CMP_STR(2,usage);
        if (size <= 0) CMP_ERR("create_buffer: size must be positive");
        if ((unsigned long)size > CMP_MAX_BUFFER_BYTES)
            CMP_ERR("create_buffer: size exceeds the 1 GiB per-buffer limit");
        if (strcmp(usage,"storage") && strcmp(usage,"uniform") &&
            strcmp(usage,"transfer") && strcmp(usage,"indirect"))
            CMP_ERR("create_buffer: usage must be storage, uniform, transfer or indirect");
        int slot = -1;
        for (int i = 0; i < c->nbufs; i++) if (!c->bufs[i].alive) { slot = i; break; }
        if (slot < 0) CMP_ERR("create_buffer: too many live buffers");
        CmpBuffer *b = &c->bufs[slot];
        memset(b, 0, sizeof(*b));
        b->host = (unsigned char *)calloc(1, (size_t)size);
        if (!b->host) CMP_ERR("create_buffer: out of memory");
        b->size = (size_t)size;
        snprintf(b->usage, sizeof(b->usage), "%s", usage);
#ifdef FLUXA_COMPUTE_VULKAN
        if (!cmp_vk_make_buffer(c, b, errbuf, sizeof(errbuf))) {
            free(b->host); b->host = NULL;
            errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
            *had_error=1; return cmp_nil();
        }
#endif
        b->alive = 1;
        return cmp_int(CMP_MAKE_HANDLE(c->generation, slot));
    }

    if (!strcmp(fn_name,"destroy_buffer")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_BUF(c,1,b);
#ifdef FLUXA_COMPUTE_VULKAN
        cmp_vk_free_buffer(c, b);
#endif
        free(b->host); b->host = NULL;
        b->alive = 0;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"buffer_size")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_BUF(c,1,b);
        return cmp_int((long)b->size);
    }

    /* compute.upload(ctx, buffer, arr, offset) → nil
     * Takes an int arr or a float arr and packs it into the contiguous 32-bit
     * block a shader reads: int32 or float32. Fluxa arrays are arrays of
     * Value, so this conversion has to happen somewhere; doing it here keeps
     * it out of every script. */
    if (!strcmp(fn_name,"upload")) {
        CMP_NEED(4); CMP_CTX(0,c); CMP_BUF(c,1,b);
        if (args[2].type != VAL_ARR || !args[2].as.arr.data)
            CMP_ERR("upload: expected an int arr or a float arr");
        CMP_INT(3,offset);
        if (offset < 0) CMP_ERR("upload: offset must not be negative");
        int n = args[2].as.arr.size;
        if (n <= 0) CMP_ERR("upload: the array is empty");
        size_t need = (size_t)n * 4u;
        if ((size_t)offset > b->size || need > b->size - (size_t)offset)
            CMP_ERR("upload: the data does not fit in the buffer at that offset");
        /* One element decides the whole array's type — a mixed array would
         * produce a block the shader cannot interpret, so it is rejected. */
        ValType t0 = args[2].as.arr.data[0].type;
        if (t0 != VAL_INT && t0 != VAL_FLOAT)
            CMP_ERR("upload: expected an int arr or a float arr");
        unsigned char *dst = b->host + offset;
        for (int i = 0; i < n; i++) {
            Value *e = &args[2].as.arr.data[i];
            if (e->type != t0)
                CMP_ERR("upload: the array mixes int and float — a shader reads "
                        "one packed type, so the array must be uniform");
            if (t0 == VAL_INT) {
                int32_t v = (int32_t)e->as.integer;
                memcpy(dst + (size_t)i*4u, &v, 4);
            } else {
                float v = (float)e->as.real;
                memcpy(dst + (size_t)i*4u, &v, 4);
            }
        }
#ifdef FLUXA_COMPUTE_VULKAN
        if (!cmp_vk_upload(c, b, (size_t)offset, need, errbuf, sizeof(errbuf))) {
            errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
            *had_error=1; return cmp_nil();
        }
#endif
        return cmp_nil();
    }

    /* compute.download(ctx, buffer, offset, size) → dyn
     * Returns floats — the form a result is usually read in. Allocates the dyn
     * it returns, so in a simulation loop free() it each turn: the collector
     * runs at a safe point and a loop never reaches one. */
    if (!strcmp(fn_name,"download")) {
        CMP_NEED(4); CMP_CTX(0,c); CMP_BUF(c,1,b);
        CMP_INT(2,offset); CMP_INT(3,size);
        if (offset < 0 || size <= 0) CMP_ERR("download: offset and size must be positive");
        if ((size_t)offset > b->size || (size_t)size > b->size - (size_t)offset)
            CMP_ERR("download: the requested region falls outside the buffer");
        if (size % 4 != 0) CMP_ERR("download: size must be a multiple of 4 bytes");
#ifdef FLUXA_COMPUTE_VULKAN
        if (!cmp_vk_download(c, b, (size_t)offset, (size_t)size, errbuf, sizeof(errbuf))) {
            errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
            *had_error=1; return cmp_nil();
        }
#endif
        int n = (int)(size / 4);
        FluxaDyn *d = (FluxaDyn *)malloc(sizeof(FluxaDyn));
        if (!d) CMP_ERR("download: out of memory");
        memset(d, 0, sizeof(*d));
        d->items = (Value *)malloc(sizeof(Value) * (size_t)n);
        if (!d->items) { free(d); CMP_ERR("download: out of memory"); }
        for (int i = 0; i < n; i++) {
            float f;
            memcpy(&f, b->host + offset + (size_t)i*4u, 4);
            d->items[i] = cmp_flt((double)f);
        }
        d->count = n; d->cap = n;
        Value v; v.type = VAL_DYN; v.as.dyn = d; return v;
    }

    if (!strcmp(fn_name,"copy_buffer")) {
        CMP_NEED(6); CMP_CTX(0,c); CMP_BUF(c,1,src); CMP_BUF(c,2,dst);
        CMP_INT(3,size); CMP_INT(4,soff); CMP_INT(5,doff);
        if (size <= 0)        CMP_ERR("copy_buffer: size must be positive");
        if (soff < 0 || doff < 0) CMP_ERR("copy_buffer: offsets must not be negative");
        if ((size_t)soff > src->size || (size_t)size > src->size - (size_t)soff)
            CMP_ERR("copy_buffer: the source region falls outside the source buffer");
        if ((size_t)doff > dst->size || (size_t)size > dst->size - (size_t)doff)
            CMP_ERR("copy_buffer: the destination region falls outside the destination buffer");
        /* memmove, not memcpy: src and dst may be the same buffer. */
        memmove(dst->host + doff, src->host + soff, (size_t)size);
#ifdef FLUXA_COMPUTE_VULKAN
        if (!cmp_vk_copy(c, src, dst, (size_t)soff, (size_t)doff, (size_t)size,
                         errbuf, sizeof(errbuf))) {
            errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
            *had_error=1; return cmp_nil();
        }
#endif
        return cmp_nil();
    }

    if (!strcmp(fn_name,"fill_buffer")) {
        CMP_NEED(3); CMP_CTX(0,c); CMP_BUF(c,1,b); CMP_INT(2,val);
        int32_t v32 = (int32_t)val;
        size_t words = b->size / 4u;
        for (size_t i = 0; i < words; i++) memcpy(b->host + i*4u, &v32, 4);
#ifdef FLUXA_COMPUTE_VULKAN
        if (!cmp_vk_upload(c, b, 0, words*4u, errbuf, sizeof(errbuf))) {
            errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
            *had_error=1; return cmp_nil();
        }
#endif
        return cmp_nil();
    }

    /* ── Kernels ──────────────────────────────────────────────────── */

    if (!strcmp(fn_name,"load_kernel") || !strcmp(fn_name,"load_kernel_entry")) {
        int with_entry = !strcmp(fn_name,"load_kernel_entry");
        CMP_NEED(with_entry ? 3 : 2);
        CMP_CTX(0,c); CMP_STR(1,path);
        const char *entry = "main";
        if (with_entry) {
            if (args[2].type != VAL_STRING || !args[2].as.string)
                CMP_ERR("expected str for the entry point");
            entry = args[2].as.string;
            if (!entry[0]) CMP_ERR("the entry point name must not be empty");
        }

        /* Read and validate the SPIR-V before handing anything to a driver.
         * A shader module goes straight into the GPU driver, so the header is
         * checked here: right magic, size a multiple of 4, and within the cap.
         * A truncated or unrelated file is rejected with a clear message
         * instead of becoming the driver's problem. */
        FILE *f = fopen(path, "rb");
        if (!f) CMP_ERR("cannot open the SPIR-V file");
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); CMP_ERR("cannot size the SPIR-V file"); }
        long fsz = ftell(f);
        if (fsz <= 0) { fclose(f); CMP_ERR("the SPIR-V file is empty"); }
        if ((unsigned long)fsz > CMP_MAX_SPV_BYTES) {
            fclose(f); CMP_ERR("the SPIR-V module exceeds the size limit");
        }
        if (fsz % 4 != 0) {
            fclose(f);
            CMP_ERR("not a SPIR-V module — its size is not a multiple of 4 bytes");
        }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); CMP_ERR("cannot rewind the SPIR-V file"); }
        unsigned char *spv = (unsigned char *)malloc((size_t)fsz);
        if (!spv) { fclose(f); CMP_ERR("out of memory"); }
        if (fread(spv, 1, (size_t)fsz, f) != (size_t)fsz) {
            free(spv); fclose(f); CMP_ERR("short read on the SPIR-V file");
        }
        fclose(f);
        unsigned magic;
        memcpy(&magic, spv, 4);
        if (magic != CMP_SPV_MAGIC) {
            free(spv);
            CMP_ERR("not a SPIR-V module — wrong magic number "
                    "(compile the shader with glslangValidator -V or glslc)");
        }

        int slot = -1;
        for (int i = 0; i < c->nkerns; i++) if (!c->kerns[i].alive) { slot = i; break; }
        if (slot < 0) { free(spv); CMP_ERR("too many live kernels"); }
        CmpKernel *k = &c->kerns[slot];
        memset(k, 0, sizeof(*k));
        k->path  = (char *)malloc(strlen(path) + 1);
        k->entry = (char *)malloc(strlen(entry) + 1);
        if (!k->path || !k->entry) {
            free(k->path); free(k->entry); free(spv);
            CMP_ERR("out of memory");
        }
        strcpy(k->path, path);
        strcpy(k->entry, entry);
#ifdef FLUXA_COMPUTE_VULKAN
        if (!cmp_vk_make_kernel(c, k, spv, (size_t)fsz, errbuf, sizeof(errbuf))) {
            free(k->path); free(k->entry); free(spv);
            errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
            *had_error=1; return cmp_nil();
        }
#endif
        free(spv);
        k->alive = 1;
        return cmp_int(CMP_MAKE_HANDLE(c->generation, slot));
    }

    if (!strcmp(fn_name,"destroy_kernel")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_KERN(c,1,k);
#ifdef FLUXA_COMPUTE_VULKAN
        cmp_vk_free_kernel(c, k);
#endif
        /* Unbind it if it was the current kernel: leaving a dangling bound
         * handle would turn the next dispatch into a confusing error rather
         * than an obvious one. */
        if (c->bound_kernel == _kk) c->bound_kernel = 0;
        free(k->path);  k->path = NULL;
        free(k->entry); k->entry = NULL;
        k->alive = 0;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"bind_kernel")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_KERN(c,1,k);
        (void)k;
        c->bound_kernel = _kk;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"bind_buffer") || !strcmp(fn_name,"bind_uniform")) {
        CMP_NEED(3); CMP_CTX(0,c); CMP_INT(1,slot);
        if (slot < 0 || slot > CMP_MAX_SLOT) CMP_ERR("slot must be between 0 and 31");
        CMP_BUF(c,2,b);
        (void)b;
        c->bound_slots[slot] = _hb;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"push_constants")) {
        CMP_NEED(3); CMP_CTX(0,c);
        if (args[1].type != VAL_ARR || !args[1].as.arr.data)
            CMP_ERR("push_constants: expected an int arr or a float arr");
        CMP_INT(2,size);
        if (size <= 0) CMP_ERR("push_constants: size must be positive");
        if (size > CMP_MAX_PUSH_BYTES)
            CMP_ERR("push_constants: Vulkan guarantees only 128 bytes of push constants");
        if (size % 4 != 0) CMP_ERR("push_constants: size must be a multiple of 4 bytes");
        if (args[1].as.arr.size * 4 < size)
            CMP_ERR("push_constants: the array is smaller than the size requested");
        ValType pt = args[1].as.arr.data[0].type;
        if (pt != VAL_INT && pt != VAL_FLOAT)
            CMP_ERR("push_constants: expected an int arr or a float arr");
        int pn = (int)(size / 4);
        for (int i = 0; i < pn; i++) {
            Value *e = &args[1].as.arr.data[i];
            if (e->type != pt)
                CMP_ERR("push_constants: the array mixes int and float");
            if (pt == VAL_INT) {
                int32_t v = (int32_t)e->as.integer;
                memcpy(c->push_data + (size_t)i*4u, &v, 4);
            } else {
                float v = (float)e->as.real;
                memcpy(c->push_data + (size_t)i*4u, &v, 4);
            }
        }
        c->push_size = (int)size;
        return cmp_nil();
    }

    /* ── Recording and execution ──────────────────────────────────── */

    if (!strcmp(fn_name,"begin")) {
        CMP_NEED(1); CMP_CTX(0,c);
        if (c->recording) CMP_ERR("begin: a batch is already open — call compute.end() first");
        c->recording  = 1;
        c->dispatches = 0;
        c->nrec       = 0;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"dispatch")) {
        CMP_NEED(4); CMP_CTX(0,c);
        CMP_INT(1,gx); CMP_INT(2,gy); CMP_INT(3,gz);
        if (!c->recording)
            CMP_ERR("dispatch: no batch is open — call compute.begin() first");
        if (!c->bound_kernel)
            CMP_ERR("dispatch: no kernel is bound — call compute.bind_kernel() first");
        if (gx <= 0 || gy <= 0 || gz <= 0)
            CMP_ERR("dispatch: the workgroup counts must all be positive");
        if (c->nrec >= CMP_MAX_RECORDED)
            CMP_ERR("dispatch: too many steps in one batch — split it with "
                    "compute.end() and compute.begin()");
#ifdef FLUXA_COMPUTE_VULKAN
        if ((unsigned)gx > c->max_wg[0] || (unsigned)gy > c->max_wg[1] ||
            (unsigned)gz > c->max_wg[2])
            CMP_ERR("dispatch: workgroup count exceeds this device's limit "
                    "(see compute.max_workgroup_x/y/z)");
        cmp_vk_dispatch(c, (uint32_t)gx, (uint32_t)gy, (uint32_t)gz);
#endif
        c->dispatches++;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"barrier")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_STR(1,kind);
        if (!c->recording) CMP_ERR("barrier: no batch is open");
        if (strcmp(kind,"compute") && strcmp(kind,"compute_to_compute") &&
            strcmp(kind,"compute_to_transfer") && strcmp(kind,"transfer_to_compute"))
            CMP_ERR("barrier: expected compute, compute_to_compute, "
                    "compute_to_transfer or transfer_to_compute");
        if (c->nrec >= CMP_MAX_RECORDED)
            CMP_ERR("barrier: too many steps in one batch");
#ifdef FLUXA_COMPUTE_VULKAN
        cmp_vk_barrier(c, kind);
#endif
        return cmp_nil();
    }

    if (!strcmp(fn_name,"end")) {
        CMP_NEED(1); CMP_CTX(0,c);
        if (!c->recording) CMP_ERR("end: no batch is open");
        c->recording = 0;
        long ticket  = c->next_ticket++;
        c->last_ticket = ticket;
#ifdef FLUXA_COMPUTE_VULKAN
        if (!cmp_vk_submit(c, errbuf, sizeof(errbuf))) {
            errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
            *had_error=1; return cmp_nil();
        }
#else
        /* Without a device the batch is finished the moment it is submitted.
         * ready() therefore answers true immediately, which keeps a program's
         * ticket logic exercisable in a headless test. */
        c->completed_ticket = ticket;
#endif
        return cmp_int(ticket);
    }

    /* ── Tickets ──────────────────────────────────────────────────── */

    if (!strcmp(fn_name,"ready") || !strcmp(fn_name,"wait")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_INT(1,ticket);
        if (ticket <= 0 || ticket >= c->next_ticket)
            CMP_ERR("this ticket was never issued by this context");
#ifdef FLUXA_COMPUTE_VULKAN
        if (!strcmp(fn_name,"wait")) cmp_vk_wait(c, ticket);
        else return cmp_bool(cmp_vk_ready(c, ticket));
#endif
        if (!strcmp(fn_name,"wait")) {
            if (c->completed_ticket < ticket) c->completed_ticket = ticket;
            return cmp_nil();
        }
        return cmp_bool(c->completed_ticket >= ticket);
    }

    if (!strcmp(fn_name,"last_ticket")) {
        CMP_NEED(1); CMP_CTX(0,c);
        return cmp_int(c->last_ticket);
    }

    /* ── Limits ───────────────────────────────────────────────────── */

    if (!strcmp(fn_name,"max_workgroup_x") || !strcmp(fn_name,"max_workgroup_y") ||
        !strcmp(fn_name,"max_workgroup_z")) {
        CMP_NEED(1); CMP_CTX(0,c);
        int axis = fn_name[strlen(fn_name)-1] == 'x' ? 0 :
                   fn_name[strlen(fn_name)-1] == 'y' ? 1 : 2;
#ifdef FLUXA_COMPUTE_VULKAN
        return cmp_int((long)c->max_wg[axis]);
#else
        (void)c; (void)axis;
        return cmp_int(65535);   /* the minimum Vulkan requires of any device */
#endif
    }

    if (!strcmp(fn_name,"max_buffer_size")) {
        CMP_NEED(1); CMP_CTX(0,c);
#ifdef FLUXA_COMPUTE_VULKAN
        return cmp_int((long)c->max_alloc);
#else
        (void)c;
        return cmp_int((long)CMP_MAX_BUFFER_BYTES);
#endif
    }

    if (!strcmp(fn_name,"memory_total")) {
        CMP_NEED(1); CMP_CTX(0,c);
#ifdef FLUXA_COMPUTE_VULKAN
        return cmp_int((long)c->mem_total);
#else
        (void)c;
        return cmp_int(0);   /* 0 means "not reported", never a guess */
#endif
    }

    /* ── Profiling ────────────────────────────────────────────────── */

    if (!strcmp(fn_name,"profile_begin")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_STR(1,name);
        if (c->profiling) CMP_ERR("profile_begin: a region is already open");
        snprintf(c->profile_name, sizeof(c->profile_name), "%s", name);
        c->profiling = 1;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"profile_end")) {
        CMP_NEED(1); CMP_CTX(0,c);
        if (!c->profiling) CMP_ERR("profile_end: no region is open");
        c->profiling = 0;
        return cmp_nil();
    }

    if (!strcmp(fn_name,"profile_ms")) {
        CMP_NEED(2); CMP_CTX(0,c); CMP_STR(1,name);
        if (strcmp(name, c->profile_name))
            CMP_ERR("profile_ms: no completed region by that name");
#ifdef FLUXA_COMPUTE_VULKAN
        return cmp_flt(cmp_vk_profile_ms(c));
#else
        /* -1.0 means "not measured", which a script can test. Returning 0.0
         * would read as an instantaneous kernel. */
        return cmp_flt(-1.0);
#endif
    }

    snprintf(errbuf,sizeof(errbuf),"compute.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"compute",line);
    *had_error=1;
    return cmp_nil();
}

#undef CMP_ERR
#undef CMP_NEED
#undef CMP_STR
#undef CMP_INT
#undef CMP_CTX
#undef CMP_BUF
#undef CMP_KERN

FLUXA_LIB_EXPORT(
    name     = "compute",
    toml_key = "std.compute",
    owner    = "compute",
    call     = fluxa_std_compute_call,
    rt_aware = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_COMPUTE_H */
