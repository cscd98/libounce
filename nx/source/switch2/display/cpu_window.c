/* SPDX-License-Identifier: ISC
 * A display window for CPU-written images.
 *
 * Switch 1's framebuffer gives nvmap-registered CPU memory to a vi buffer
 * queue. On Switch 2, this window registers the same memory with the GSV
 * GPU worker and imports it as GSV display images. The VI2 layer of the
 * window's NWindow (nwindowGetVi2Layer) presents them. There is no GPU mapping
 * and no GPU work.
 *
 * A process has one GPU session for these windows: the GSV sync and GPU
 * objects, the worker with its slot event, the client and the address space.
 * The windows share it, and it closes with the last of them. Each window has
 * its own layer, backing and images: the default NWindow's, and one for each
 * NWindow of nwindowCreateForDisplay that has a framebuffer.
 */
#include <string.h>
#include "cpu_window.h"
#include "vi2_window.h"
#include "kernel/mutex.h"
#include "kernel/svc.h"
#include "switch2/kernel/svc.h"
#include "arm/counter.h"
#include "services/nv.h"

// Turing Color2D GOB: 64 bytes by 8 rows of 16-byte lines, in blocks of 16
// GOBs (128 rows). See NIL src/nouveau/nil/tiling.rs and copy.rs
// CopyGOBTuring2D. Layout block format 1 selects Turing sector order.
#define GOB_WIDTH_B        64
#define GOB_HEIGHT         8
#define GOB_SIZE_B         (GOB_WIDTH_B * GOB_HEIGHT)
#define BLOCK_HEIGHT_LOG2  7    // rows: 16 GOBs
#define BLOCK_HEIGHT       (1u << BLOCK_HEIGHT_LOG2)
#define BLOCK_SIZE_B       (GOB_SIZE_B * (BLOCK_HEIGHT / GOB_HEIGHT))

#define IMAGE_FORMAT_RGBA8         1
#define IMAGE_BLOCK_FORMAT_TURING  1
// Image memory type for display images.
#define IMAGE_MEMORY_TYPE          3

// Frame mode a game's startup registers.
#define FRAME_MODE   2
// Main-session slot of the worker.
#define WORKER_SLOT  0
// GPU client name "libnxfb", packed little-endian, for gsvGpuRegisterClient.
#define CLIENT_NAME  0x006266786e62696cull
// SVC 0x80 attribute set on the backing before gsvGpuRegisterMemory.
#define BACKING_PREPARE_BIT  0x20
#define BACKING_ALIGN        0x10000

// The game AS import on /dev/nvhost-as-gpu: version 5, the VM ID at +4 and
// the user interval [0x08000000, 0xc000000000) at +16.
#define AS_IMPORT_IOCTL    0x40284109
#define AS_IMPORT_VERSION  5
#define AS_USER_FIRST      0x08000000ull
#define AS_USER_END        0xc000000000ull

// A present without this flag unregisters the layer's image.
#define PRESENT_IMAGE_FLAG  1ull

#define WAIT_LIMIT_NS  2000000000ull
#define POLL_NS        1000000ull

// The GPU session that the windows share.
typedef struct {
    u32 users;              // Open windows.
    GsvSync sync;
    GsvGpu gpu;
    Event slot_event;
    u8 client;
    u32 as_fd;
} CpuWindowSession;

// Windows of NWindows other than the default one, as nwindowCreateForDisplay allows.
#define CPUWIN_MAX_DISPLAY_WINDOWS 4

static Mutex g_sessionMutex;        // The session's users.
static CpuWindowSession g_session;
static Mutex g_windowsMutex;        // The table of the display windows.
// The worker signals one slot event for every window's backing. A release
// clears it, asks and waits, one window at a time.
static Mutex g_releaseMutex;

static CpuWindow g_defaultWindow;
static CpuWindow g_displayWindows[CPUWIN_MAX_DISPLAY_WINDOWS];

static u32 _cpuwinPitch(u32 width) {
    return (width * 4 + GOB_WIDTH_B - 1) & ~(GOB_WIDTH_B - 1);
}

static u64 _cpuwinImageSize(u32 width, u32 height) {
    const u32 height_aligned = (height + BLOCK_HEIGHT - 1) & ~(BLOCK_HEIGHT - 1);
    return (u64)_cpuwinPitch(width) * height_aligned;
}

static void _cpuwinMakeLayout(GsvImageLayout* out, u32 width, u32 height) {
    gsvMakeImageLayout(out, width, height, BLOCK_HEIGHT_LOG2,
                       _cpuwinPitch(width) / GOB_WIDTH_B, IMAGE_FORMAT_RGBA8,
                       IMAGE_BLOCK_FORMAT_TURING, 0);
}

u32 cpuwinPixelOffset(u32 width, u32 x, u32 y) {
    const u32 x_b = x * 4;
    const u32 blocks_per_row = _cpuwinPitch(width) / GOB_WIDTH_B;
    const u32 block = (y / BLOCK_HEIGHT) * blocks_per_row + x_b / GOB_WIDTH_B;
    const u32 gob = (y % BLOCK_HEIGHT) / GOB_HEIGHT;
    // Inside the GOB: two 32-byte halves, each 4x4 sectors of 16-byte lines.
    const u32 gx = x_b % GOB_WIDTH_B, gy = y % GOB_HEIGHT;
    return block * BLOCK_SIZE_B + gob * GOB_SIZE_B +
           (gx / 32) * 0x100 + (gy / 4) * 0x80 + ((gx % 32) / 16) * 0x40 +
           (gy % 4) * 0x10 + (gx % 16);
}

// A VI2 release: the complemented counter ID at +0, the mode at +4 and the
// threshold at +8. All are zero when unused.
static u32 _cpuwinReleaseToken(const Vi2Dependency* d) {
    u32 token;
    memcpy(&token, d->data, sizeof(token));
    return token;
}

static u64 _cpuwinReleaseThreshold(const Vi2Dependency* d) {
    u64 threshold;
    memcpy(&threshold, d->data + 8, sizeof(threshold));
    return threshold;
}

static bool _cpuwinReleaseEmpty(const Vi2Dependency* d) {
    return !_cpuwinReleaseToken(d) && !d->data[4] && !_cpuwinReleaseThreshold(d);
}

// Waits for a release through the GSV sync counter it names
// (gsvSyncReadCounter).
static Result _cpuwinWaitRelease(CpuWindow* w, const Vi2Dependency* d) {
    if (_cpuwinReleaseEmpty(d))
        return 0;
    const u64 start = armGetSystemTick();
    for (;;) {
        u64 counter = 0;
        u8 mode = 0;
        Result rc = gsvSyncReadCounter(&g_session.sync, ~_cpuwinReleaseToken(d), &counter, &mode);
        if (R_FAILED(rc))
            return rc;
        if (mode != 1)
            return MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer);
        if (counter >= _cpuwinReleaseThreshold(d))
            return 0;
        if (armTicksToNs(armGetSystemTick() - start) > WAIT_LIMIT_NS)
            return KERNELRESULT(TimedOut);
        svcSleepThread(POLL_NS);
    }
}

// Gets a free VI2 slot. Consume the layer's availability notification,
// query, and wait for the next notification.
static Result _cpuwinAcquireSlot(CpuWindow* w, u8* out_slot) {
    const u64 start = armGetSystemTick();
    vi2WaitLayerAvailability(w->layer, 0);
    for (;;) {
        u64 available = 0;
        Result rc = vi2GetLayerAvailability(w->layer, &available);
        if (R_FAILED(rc))
            return rc;
        if (available) {
            *out_slot = (u8)__builtin_ctzll(available);
            return 0;
        }
        if (armTicksToNs(armGetSystemTick() - start) > WAIT_LIMIT_NS)
            return KERNELRESULT(TimedOut);
        vi2WaitLayerAvailability(w->layer, 100 * POLL_NS);
    }
}

// GSV root, nvservices, sync and frame registration, then the GPU session,
// its worker and a client.
static Result _cpuwinOpenGpu(void) {
    Result rc = gsvInitialize();
    if (R_FAILED(rc))
        return rc;
    rc = nvInitialize();
    if (R_FAILED(rc))
        goto fail_gsv;
    rc = gsvOpenSync(&g_session.sync);
    if (R_FAILED(rc))
        goto fail_nv;
    rc = gsvRegisterFrame(FRAME_MODE, NULL);
    if (R_FAILED(rc))
        goto fail_sync;
    rc = gsvOpenGpu(&g_session.gpu);
    if (R_FAILED(rc))
        goto fail_sync;
    rc = gsvGpuCreateSlotEvent(&g_session.gpu, WORKER_SLOT, &g_session.slot_event, false);
    if (R_FAILED(rc))
        goto fail_gpu;
    rc = gsvGpuOpenWorker(&g_session.gpu, WORKER_SLOT, &g_session.slot_event);
    if (R_FAILED(rc))
        goto fail_slot_event;
    rc = gsvGpuRegisterClient(&g_session.gpu, CLIENT_NAME, &g_session.client);
    if (R_FAILED(rc))
        goto fail_slot_event;
    return 0;

fail_slot_event:
    eventClose(&g_session.slot_event);
fail_gpu:
    gsvGpuClose(&g_session.gpu);
fail_sync:
    gsvSyncClose(&g_session.sync);
fail_nv:
    nvExit();
fail_gsv:
    gsvExit();
    return rc;
}

static void _cpuwinCloseGpu(void) {
    gsvGpuReleaseClient(&g_session.gpu, g_session.client);
    gsvSyncClose(&g_session.sync);
    eventClose(&g_session.slot_event);
    gsvGpuClose(&g_session.gpu);
    nvExit();
    gsvExit();
}

// A VM registered with the root and imported on nvhost-as-gpu. Without the
// import, gsvRegisterImageMemory returns 0x280f3.
static Result _cpuwinOpenAddressSpace(void) {
    u32 vm_id = 0;
    Result rc = gsvGpuCreateVm(&g_session.gpu, &vm_id);
    GsvGpuVaRange user[4];
    u32 user_count = 0;
    if (R_SUCCEEDED(rc))
        rc = gsvGpuGetVmUserRanges(&g_session.gpu, vm_id, user, &user_count);
    GsvGpuVaRange gsv_range;
    if (R_SUCCEEDED(rc))
        rc = gsvGpuGetVmGsvRange(&g_session.gpu, vm_id, &gsv_range);
    if (R_SUCCEEDED(rc))
        rc = gsvRegisterVm(vm_id);
    if (R_SUCCEEDED(rc))
        rc = nvOpen(&g_session.as_fd, "/dev/nvhost-as-gpu");
    if (R_FAILED(rc))
        return rc;

    struct {
        u32 version;
        u32 vm_id;
        u64 reserved;
        u64 user_first;
        u64 user_end;
        u64 reserved2;
    } argument = { AS_IMPORT_VERSION, vm_id, 0, AS_USER_FIRST, AS_USER_END, 0 };
    _Static_assert(sizeof(argument) == 40, "AS import argument size");
    rc = nvIoctl(g_session.as_fd, AS_IMPORT_IOCTL, &argument);
    if (R_FAILED(rc))
        nvClose(g_session.as_fd);
    return rc;
}

// Takes a user of the session, opening it for the first one.
static Result _cpuwinAcquireSession(void) {
    Result rc = 0;
    mutexLock(&g_sessionMutex);
    if (!g_session.users) {
        rc = _cpuwinOpenGpu();
        if (R_SUCCEEDED(rc)) {
            rc = _cpuwinOpenAddressSpace();
            if (R_FAILED(rc))
                _cpuwinCloseGpu();
        }
    }
    if (R_SUCCEEDED(rc))
        g_session.users++;
    mutexUnlock(&g_sessionMutex);
    return rc;
}

static void _cpuwinReleaseSession(void) {
    mutexLock(&g_sessionMutex);
    if (--g_session.users == 0) {
        nvClose(g_session.as_fd);
        _cpuwinCloseGpu();
        memset(&g_session, 0, sizeof(g_session));
    }
    mutexUnlock(&g_sessionMutex);
}

Result cpuwinOpen(CpuWindow* w, NWindow* nw) {
    memset(w, 0, sizeof(*w));
    Result rc = _cpuwinAcquireSession();
    if (R_FAILED(rc))
        return rc;
    // The NWindow owns the layer (default_window.c, vi2_window.c).
    rc = nwindowGetVi2Layer(nw, &w->layer);
    if (R_FAILED(rc)) {
        _cpuwinReleaseSession();
        return rc;
    }
    w->nw = nw;
    w->is_open = true;
    return 0;
}

void cpuwinClose(CpuWindow* w) {
    if (!w->is_open)
        return;
    _cpuwinReleaseSession();
    memset(w, 0, sizeof(*w));
}

Result cpuwinGetImageSize(u32 width, u32 height, u64* out_size) {
    GsvImageLayout layout;
    _cpuwinMakeLayout(&layout, width, height);
    u64 size = 0;
    Result rc = gsvGetImageSize(&layout, &size);
    if (R_FAILED(rc))
        return rc;
    // cpuwinPixelOffset writes this layout. GSV must agree on its size.
    if (size != _cpuwinImageSize(width, height))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    *out_size = size;
    return 0;
}

// Imports one image of the backing: gsvReserveImageMemory,
// gsvRegisterImageMemory, gsvCreateImage and gsvCreateDisplayAlias.
static Result _cpuwinCreateImage(CpuWindow* w, u32 index, u64 image_size) {
    CpuWindowImage* image = &w->images[index];
    GsvImageLayout layout;
    _cpuwinMakeLayout(&layout, w->width, w->height);
    const u64 offset = index * image_size;
    Result rc = gsvReserveImageMemory(IMAGE_MEMORY_TYPE, image_size, &image->memory_id);
    if (R_FAILED(rc))
        goto fail;
    rc = gsvRegisterImageMemory(image->memory_id, (uintptr_t)w->backing + offset,
                                image_size, w->range_id, offset);
    if (R_FAILED(rc))
        goto fail_memory;
    rc = gsvCreateImage(&layout, image->memory_id, &image->image_id);
    if (R_FAILED(rc))
        goto fail_memory;
    rc = gsvCreateDisplayAlias(image->image_id, 1);
    if (R_FAILED(rc))
        goto fail_image;
    return 0;

fail_image:
    gsvDestroyImage(image->image_id);
fail_memory:
    gsvReleaseImageMemory(image->memory_id);
fail:
    memset(image, 0, sizeof(*image));
    return rc;
}

static void _cpuwinDestroyImage(CpuWindowImage* image) {
    gsvDestroyDisplayAlias(image->image_id);
    gsvDestroyImage(image->image_id);
    gsvReleaseImageMemory(image->memory_id);
    memset(image, 0, sizeof(*image));
}

// Registers the backing with the worker, CPU-cached. The steps are SVC 0x80
// prepare, then gsvGpuRegisterMemory.
static Result _cpuwinRegisterBacking(CpuWindow* w) {
    Result rc = svcUnknown80(w->backing, w->backing_size, BACKING_PREPARE_BIT, BACKING_PREPARE_BIT);
    if (R_FAILED(rc))
        return rc;
    rc = gsvGpuRegisterMemory(&g_session.gpu, (uintptr_t)w->backing, w->backing_size, g_session.client, &w->range_id);
    if (R_SUCCEEDED(rc) && !w->range_id)
        rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_FAILED(rc))
        svcUnknown80(w->backing, w->backing_size, BACKING_PREPARE_BIT, 0);
    return rc;
}

// The worker signals the slot event when it retires the range. Then this
// function clears the SVC 0x80 attribute.
static Result _cpuwinUnregisterBacking(CpuWindow* w) {
    mutexLock(&g_releaseMutex);
    // The event stays signalled after an earlier release. Clearing an event
    // that is not signalled returns InvalidState.
    Result rc = eventClear(&g_session.slot_event);
    if (R_VALUE(rc) == KERNELRESULT(InvalidState))
        rc = 0;
    if (R_SUCCEEDED(rc))
        rc = gsvGpuUnregisterMemory(&g_session.gpu, w->range_id, WORKER_SLOT);
    if (R_SUCCEEDED(rc))
        rc = eventWait(&g_session.slot_event, WAIT_LIMIT_NS);
    mutexUnlock(&g_releaseMutex);
    if (R_SUCCEEDED(rc))
        rc = svcUnknown80(w->backing, w->backing_size, BACKING_PREPARE_BIT, 0);
    return rc;
}

Result cpuwinCreateImages(CpuWindow* w, void* backing, u64 backing_size, u32 width, u32 height, u32 num_images) {
    if (!w->is_open || w->num_images)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!backing || ((uintptr_t)backing & (BACKING_ALIGN - 1)) || !num_images || num_images > CPUWIN_MAX_IMAGES)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    u64 image_size = 0;
    Result rc = cpuwinGetImageSize(width, height, &image_size);
    if (R_FAILED(rc))
        return rc;
    if (backing_size < num_images * image_size || (backing_size & 0xFFF))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    w->backing = backing;
    w->backing_size = backing_size;
    w->width = width;
    w->height = height;
    rc = _cpuwinRegisterBacking(w);
    if (R_FAILED(rc))
        return rc;
    for (u32 i = 0; i < num_images && R_SUCCEEDED(rc); i++) {
        rc = _cpuwinCreateImage(w, i, image_size);
        if (R_SUCCEEDED(rc))
            w->num_images = i + 1;
    }
    if (R_FAILED(rc)) {
        // No present used these images.
        for (u32 i = 0; i < w->num_images; i++)
            _cpuwinDestroyImage(&w->images[i]);
        w->num_images = 0;
        _cpuwinUnregisterBacking(w);
    }
    return rc;
}

// Retires the images. The steps are:
// 1. A present without the image flag (layer-wide). Its release must be empty.
// 2. Every image's last release.
// 3. The images and the backing registration.
Result cpuwinDestroyImages(CpuWindow* w) {
    if (!w->num_images)
        return 0;
    u8 slot = 0;
    Result rc = _cpuwinAcquireSlot(w, &slot);
    if (R_SUCCEEDED(rc)) {
        Vi2PresentOptions options;
        vi2MakeBasicPresentOptions(&options, w->width, w->height, armGetSystemTick());
        options.flags &= ~PRESENT_IMAGE_FLAG;
        u64 available = 0;
        Vi2Dependency release = {0};
        rc = vi2UnregisterLayerImage(w->layer, slot, &options, &available, &release);
        if (R_SUCCEEDED(rc) && !_cpuwinReleaseEmpty(&release))
            rc = MAKERESULT(Module_Libnx, LibnxError_BadGfxQueueBuffer);
    }
    for (u32 i = 0; i < w->num_images && R_SUCCEEDED(rc); i++)
        rc = _cpuwinWaitRelease(w, &w->images[i].release);
    // On failure, the display may still read the images. Keep them and
    // their backing.
    if (R_FAILED(rc))
        return rc;

    for (u32 i = 0; i < w->num_images; i++)
        _cpuwinDestroyImage(&w->images[i]);
    w->num_images = 0;
    rc = _cpuwinUnregisterBacking(w);
    w->backing = NULL;
    w->backing_size = 0;
    return rc;
}

Result cpuwinWaitImage(CpuWindow* w, u32 index) {
    if (index >= w->num_images)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    return _cpuwinWaitRelease(w, &w->images[index].release);
}

// VI2 present with an empty producer dependency, because no GPU work
// produced the image. The returned release must be a counter release
// (mode 1).
Result cpuwinPresentImage(CpuWindow* w, u32 index) {
    if (index >= w->num_images)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    CpuWindowImage* image = &w->images[index];
    u8 slot = 0;
    Result rc = _cpuwinAcquireSlot(w, &slot);
    if (R_FAILED(rc))
        return rc;
    Vi2PresentOptions options;
    vi2MakeBasicPresentOptions(&options, w->width, w->height, armGetSystemTick());
    nwindowGetPresentInterval(w->nw, &options.interval_numerator,
                              &options.interval_denominator);
    const Vi2Dependency producer = {0};
    u64 available = 0;
    rc = vi2PresentLayer(w->layer, slot, image->image_id, &options, &producer,
                         &available, &image->release);
    if (R_SUCCEEDED(rc) && image->release.data[4] != 1)
        rc = MAKERESULT(Module_Libnx, LibnxError_BadGfxQueueBuffer);
    return rc;
}

// The window of an NWindow of nwindowCreateForDisplay, or a free one for it
// (is_open false), or NULL. Call with g_windowsMutex held.
static CpuWindow* _cpuwinFindDisplayWindow(NWindow* nw) {
    CpuWindow* free_window = NULL;
    for (u32 i = 0; i < CPUWIN_MAX_DISPLAY_WINDOWS; i++) {
        CpuWindow* w = &g_displayWindows[i];
        if (w->is_open && w->nw == nw)
            return w;
        if (!w->is_open && !free_window)
            free_window = w;
    }
    return free_window;
}

CpuWindow* cpuwinGet(NWindow* nw) {
    if (nw == nwindowGetDefault()) {
        if (!g_defaultWindow.is_open && R_FAILED(cpuwinOpen(&g_defaultWindow, nw)))
            return NULL;
        return &g_defaultWindow;
    }
    if (!_nwindowGetDisplayWindow(nw))
        return NULL;

    mutexLock(&g_windowsMutex);
    CpuWindow* w = _cpuwinFindDisplayWindow(nw);
    if (w && !w->is_open && R_FAILED(cpuwinOpen(w, nw)))
        w = NULL;
    mutexUnlock(&g_windowsMutex);
    return w;
}

bool cpuwinHasImages(NWindow* nw) {
    if (nw == nwindowGetDefault())
        return g_defaultWindow.num_images != 0;
    mutexLock(&g_windowsMutex);
    CpuWindow* w = _cpuwinFindDisplayWindow(nw);
    const bool has_images = w && w->is_open && w->nw == nw && w->num_images != 0;
    mutexUnlock(&g_windowsMutex);
    return has_images;
}

// Retires a framebuffer left open first. If the display still holds an
// image, everything stays open. Freeing memory that the display reads is
// worse than leaking the window.
bool cpuwinCloseFor(NWindow* nw) {
    CpuWindow* w = &g_defaultWindow;
    if (nw != nwindowGetDefault()) {
        mutexLock(&g_windowsMutex);
        w = _cpuwinFindDisplayWindow(nw);
        mutexUnlock(&g_windowsMutex);
        if (!w || !w->is_open || w->nw != nw)
            return true;
    }
    if (R_FAILED(cpuwinDestroyImages(w)))
        return false;
    cpuwinClose(w);
    return true;
}
