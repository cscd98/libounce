/* SPDX-License-Identifier: ISC
 * Switch 2 StreamPlay (stpl:u) client for hosting local GameShare.
 *
 * Command 0 of stpl:u opens the session that every other command uses; this
 * client keeps that session and closes the stpl:u root. All commands take an
 * inline context (CMIF version 1).
 *
 * The service writes guest input into shared memory (command 300). Each
 * guest input is a ring of 33 entries: the index of the latest entry and the
 * number of entries written, then the entries. An entry starts with a
 * sequence counter, odd while the service writes the entry, and a frame
 * number.
 *
 * The work memory (commands 9, 505 and 506) is given to the service as
 * transfer memory: one common slice, then three slices for each guest.
 */
#define NX_SERVICE_ASSUME_NON_DOMAIN
#include <string.h>
#include <stdatomic.h>
#include "../../services/service_guard.h"
#include "switch2/services/stpl.h"
#include "kernel/shmem.h"
#include "kernel/svc.h"

#define STPL_CONTEXT                1
#define STPL_SESSION_VERSION        3

// Guest input memory. Each Npad ID has a bank of three guests; a guest's
// part holds five Npad style rings, then six six-axis sensor rings. The
// three guest touch screen rings follow the banks.
#define STPL_RING_ENTRIES           33
#define STPL_GUEST_INPUT_SIZE       0xC000
#define STPL_NPAD_BANK_SIZE         (STPL_MAX_GUESTS * STPL_GUEST_INPUT_SIZE)
#define STPL_NPAD_RINGS_OFFSET      0x28
#define STPL_NPAD_RING_COUNT        5
#define STPL_SIXAXIS_RINGS_OFFSET   0x2490
#define STPL_SIXAXIS_RING_SIZE      0xE80
#define STPL_TOUCH_RINGS_OFFSET     (3 * STPL_NPAD_BANK_SIZE)
#define STPL_TOUCH_RING_SIZE        0x6000
#define STPL_GUEST_INPUT_EXTENT     (STPL_TOUCH_RINGS_OFFSET + STPL_MAX_GUESTS * STPL_TOUCH_RING_SIZE)

// A sample that changes during every read of a frame is given up, so that a
// writer that stops in the middle of an entry cannot stall the reader.
#define STPL_RING_READ_ATTEMPTS     8

// The service gives the work memory back after its session closes, which
// can be after the close command replies.
#define STPL_WORK_MEMORY_RETURN_ATTEMPTS 100
#define STPL_WORK_MEMORY_RETURN_POLL_NS  10000000LL

typedef struct {
    u64 latest;                                 // Index of the newest entry.
    u64 count;                                  // Number of entries written.
} StplRingHeader;

typedef struct {
    u64 sequence;                               // Odd while the service writes the entry.
    u64 frame;
} StplRingEntryHeader;

typedef struct {
    StplRingHeader header;
    struct {
        StplRingEntryHeader header;
        HidNpadCommonState state;
    } entries[STPL_RING_ENTRIES];
} StplNpadRing;

_Static_assert(sizeof(StplNpadRing) == 0x748, "StplNpadRing size");
_Static_assert(STPL_NPAD_RINGS_OFFSET + STPL_NPAD_RING_COUNT * sizeof(StplNpadRing) <= STPL_SIXAXIS_RINGS_OFFSET,
               "Npad rings overlap the six-axis rings");
_Static_assert(sizeof(StplRingHeader) + STPL_RING_ENTRIES * (sizeof(StplRingEntryHeader) + sizeof(HidSixAxisSensorState))
               == STPL_SIXAXIS_RING_SIZE, "six-axis ring size");
_Static_assert(STPL_SIXAXIS_RINGS_OFFSET + 6 * STPL_SIXAXIS_RING_SIZE <= STPL_GUEST_INPUT_SIZE,
               "six-axis rings exceed a guest's input memory");
_Static_assert(sizeof(StplRingHeader) + STPL_RING_ENTRIES * (sizeof(StplRingEntryHeader) + sizeof(HidTouchScreenState))
               <= STPL_TOUCH_RING_SIZE, "touch screen ring size");
_Static_assert(sizeof(StplConnectionState) == 8, "StplConnectionState size");
_Static_assert(sizeof(StplMatchmakingArgument) == 0x80, "StplMatchmakingArgument size");

static Service g_stplSrv;
static SharedMemory g_stplGuestInputMemory;
static const u8* g_stplGuestInput;

NX_GENERATE_SERVICE_GUARD(stpl);

static Result _stplOpenSession(void) {
    Service root;
    Result rc = smGetService(&root, "stpl:u");
    if (R_FAILED(rc))
        return rc;

    const u32 version = STPL_SESSION_VERSION;
    rc = serviceDispatchIn(&root, 0, version,
        .context = STPL_CONTEXT,
        .out_num_objects = 1,
        .out_objects = &g_stplSrv,
    );
    serviceClose(&root);
    return rc;
}

static Result _stplMapGuestInput(void) {
    struct {
        u64 size;
        u64 offset;
    } out;
    Handle memory = INVALID_HANDLE;
    Result rc = serviceDispatchOut(&g_stplSrv, 300, out,
        .context = STPL_CONTEXT,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &memory,
    );
    if (R_FAILED(rc))
        return rc;

    // The readers below index the memory with fixed offsets.
    if ((out.size & 0xFFF) || (out.offset & 7) || out.offset > out.size ||
        out.size - out.offset < STPL_GUEST_INPUT_EXTENT) {
        svcCloseHandle(memory);
        return MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen);
    }

    shmemLoadRemote(&g_stplGuestInputMemory, memory, out.size, Perm_R);
    rc = shmemMap(&g_stplGuestInputMemory);
    if (R_SUCCEEDED(rc))
        g_stplGuestInput = (const u8*)shmemGetAddr(&g_stplGuestInputMemory) + out.offset;
    return rc;
}

Result _stplInitialize(void) {
    Result rc = _stplOpenSession();
    if (R_SUCCEEDED(rc))
        rc = _stplMapGuestInput();
    return rc;
}

void _stplCleanup(void) {
    g_stplGuestInput = NULL;
    shmemClose(&g_stplGuestInputMemory);
    serviceClose(&g_stplSrv);
}

Service* stplGetServiceSession(void) {
    return &g_stplSrv;
}

static Result _stplCmdNoIO(u32 cmd_id) {
    return serviceDispatch(&g_stplSrv, cmd_id,
        .context = STPL_CONTEXT,
    );
}

static Result _stplCmdInU8NoOut(u8 inval, u32 cmd_id) {
    return serviceDispatchIn(&g_stplSrv, cmd_id, inval,
        .context = STPL_CONTEXT,
    );
}

static Result _stplCmdInU32NoOut(u32 inval, u32 cmd_id) {
    return serviceDispatchIn(&g_stplSrv, cmd_id, inval,
        .context = STPL_CONTEXT,
    );
}

static Result _stplCmdNoInOutU8(u8 *out, u32 cmd_id) {
    u8 tmp = 0;
    Result rc = serviceDispatchOut(&g_stplSrv, cmd_id, tmp,
        .context = STPL_CONTEXT,
    );
    if (R_SUCCEEDED(rc) && out) *out = tmp;
    return rc;
}

static Result _stplCmdNoInOutU32(u32 *out, u32 cmd_id) {
    u32 tmp = 0;
    Result rc = serviceDispatchOut(&g_stplSrv, cmd_id, tmp,
        .context = STPL_CONTEXT,
    );
    if (R_SUCCEEDED(rc) && out) *out = tmp;
    return rc;
}

static Result _stplCmdInU8OutU32(u8 inval, u32 *out, u32 cmd_id) {
    u32 tmp = 0;
    Result rc = serviceDispatchInOut(&g_stplSrv, cmd_id, inval, tmp,
        .context = STPL_CONTEXT,
    );
    if (R_SUCCEEDED(rc) && out) *out = tmp;
    return rc;
}

//-----------------------------------------------------------------------------
// Host session
//-----------------------------------------------------------------------------

Result stplGetRequiredMemorySize(StplStreamingMode mode, s32 guest_count, u32 *out_size) {
    const struct {
        u8 type;
        u8 mode;
        u16 reserved;
        s32 guest_count;
    } in = { 0, mode, 0, guest_count };
    u32 tmp = 0;
    Result rc = serviceDispatchInOut(&g_stplSrv, 1, in, tmp,
        .context = STPL_CONTEXT,
    );
    if (R_SUCCEEDED(rc) && out_size) *out_size = tmp;
    return rc;
}

Result stplInitializeHost(StplStreamingMode mode, s32 guest_count) {
    // The process ID is sent with the request; the service fills its field.
    const struct {
        u8 type;
        u8 mode;
        u16 reserved;
        s32 guest_count;
        u64 pid_placeholder;
    } in = { 0, mode, 0, guest_count, 0 };
    return serviceDispatchIn(&g_stplSrv, 2, in,
        .context = STPL_CONTEXT,
        .in_send_pid = true,
    );
}

Result stplFinalizeHost(void) {
    return _stplCmdNoIO(3);
}

Result stplStartGuest(s32 guest) {
    return _stplCmdInU32NoOut((u32)guest, 4);
}

Result stplCloseSession(void) {
    return _stplCmdNoIO(5);
}

Result stplGetConnectionEvent(Event *out_event, bool autoclear) {
    Handle event = INVALID_HANDLE;
    Result rc = serviceDispatch(&g_stplSrv, 7,
        .context = STPL_CONTEXT,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &event,
    );
    if (R_SUCCEEDED(rc))
        eventLoadRemote(out_event, event, autoclear);
    return rc;
}

Result stplGetConnectionState(StplConnectionState *out) {
    StplConnectionState tmp = {0};
    Result rc = serviceDispatchOut(&g_stplSrv, 8, tmp,
        .context = STPL_CONTEXT,
    );
    if (R_SUCCEEDED(rc) && out) *out = tmp;
    return rc;
}

Result stplSetGuestNpadStyleSet(u32 style_set) {
    return _stplCmdInU32NoOut(style_set, 25);
}

Result stplGetGuestNpadStyleSet(u32 *out) {
    return _stplCmdNoInOutU32(out, 26);
}

Result stplSetGuestNpadJoyHoldType(HidNpadJoyHoldType type) {
    return _stplCmdInU32NoOut(type, 27);
}

Result stplGetGuestNpadJoyHoldType(HidNpadJoyHoldType *out) {
    u32 tmp = 0;
    Result rc = _stplCmdNoInOutU32(&tmp, 28);
    if (R_SUCCEEDED(rc) && out) *out = tmp;
    return rc;
}

Result stplWriteGuestAudio(u8 guest, const void *frames, u32 frame_count, u32 *out_written) {
    const struct {
        u8 guest;
        u8 reserved[3];
        u32 frame_count;
    } in = { guest, {0}, frame_count };
    u32 tmp = 0;
    Result rc = serviceDispatchInOut(&g_stplSrv, 40, in, tmp,
        .context = STPL_CONTEXT,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcAutoSelect },
        .buffers = { { frames, (size_t)frame_count * STPL_AUDIO_FRAME_SIZE } },
    );
    if (R_SUCCEEDED(rc) && out_written) *out_written = tmp;
    return rc;
}

Result stplGetGuestAudioCapacity(u32 *out_frames) {
    return _stplCmdNoInOutU32(out_frames, 41);
}

Result stplGetGuestAudioFreeFrames(u8 guest, u32 *out_frames) {
    return _stplCmdInU8OutU32(guest, out_frames, 42);
}

Result stplGetGuestAudioQueuedFrames(u8 guest, u32 *out_frames) {
    return _stplCmdInU8OutU32(guest, out_frames, 43);
}

Result stplDisconnectGuest(u8 guest) {
    return _stplCmdInU8NoOut(guest, 70);
}

Result stplSetGuestTouchScreenRange(s32 width, s32 height) {
    const struct {
        s32 width;
        s32 height;
    } in = { width, height };
    return serviceDispatchIn(&g_stplSrv, 120, in,
        .context = STPL_CONTEXT,
    );
}

Result stplSendGuestVibrationValue(StplVibrationDeviceHandle handle, const HidVibrationValue *value) {
    const struct {
        HidVibrationValue value;
        u64 handle;
    } in = { *value, handle.handle };
    return serviceDispatchIn(&g_stplSrv, 141, in,
        .context = STPL_CONTEXT,
    );
}

Result stplSetGuestHidFeatures(u32 features) {
    return _stplCmdInU32NoOut(features, 160);
}

Result stplGetGuestHidFeatures(u32 *out) {
    return _stplCmdNoInOutU32(out, 161);
}

Result stplGetGuestNickname(u8 guest, char out[STPL_NICKNAME_LENGTH + 1]) {
    char nickname[STPL_NICKNAME_LENGTH + 1] = {0};
    Result rc = serviceDispatchInOut(&g_stplSrv, 200, guest, nickname,
        .context = STPL_CONTEXT,
    );
    if (R_SUCCEEDED(rc) && out) {
        nickname[STPL_NICKNAME_LENGTH] = 0;
        memcpy(out, nickname, sizeof(nickname));
    }
    return rc;
}

Result stplGetGuestProfileImage(u8 guest, void *buffer, size_t size, size_t *out_size) {
    u64 image_size = 0;
    Result rc = serviceDispatchInOut(&g_stplSrv, 201, guest, image_size,
        .context = STPL_CONTEXT,
        .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcAutoSelect },
        .buffers = { { buffer, size } },
    );
    if (R_SUCCEEDED(rc) && out_size) *out_size = image_size < size ? image_size : size;
    return rc;
}

Result stplStartStreaming(void) {
    return _stplCmdNoIO(500);
}

Result stplGetGuestCapacity(s32 *out) {
    u32 tmp = 0;
    Result rc = _stplCmdNoInOutU32(&tmp, 501);
    if (R_SUCCEEDED(rc) && out) *out = (s32)tmp;
    return rc;
}

Result stplGetStreamType(u8 *out) {
    return _stplCmdNoInOutU8(out, 502);
}

Result stplGetStreamingMode(StplStreamingMode *out) {
    u8 tmp = 0;
    Result rc = _stplCmdNoInOutU8(&tmp, 503);
    if (R_SUCCEEDED(rc) && out) *out = tmp;
    return rc;
}

Result stplCreateMatchmakingArgument(AccountUid uid, s32 guest_count, StplMatchmakingArgument *out) {
    const struct {
        u8 option;
        u8 guest_count;
        u8 flags[2];
        u8 reserved[8];
        u16 style_set;
        u16 reserved2;
        AccountUid uid;
    } in = { 1, (u8)guest_count, { 1, 1 }, {0}, HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld, 0, uid };
    StplMatchmakingArgument tmp;
    Result rc = serviceDispatchInOut(&g_stplSrv, 504, in, tmp,
        .context = STPL_CONTEXT,
    );
    if (R_SUCCEEDED(rc) && out) *out = tmp;
    return rc;
}

Result stplReleaseGuestWorkMemory(void) {
    return _stplCmdNoIO(507);
}

Result stplEndMatchmaking(void) {
    return _stplCmdNoIO(509);
}

Result stplSetAudioMix(const StplAudioMix *mix) {
    const struct {
        u8 auto_tune;
        u8 reserved[3];
        float speaker_volume;
        float tv_volume;
        float external_volume;
    } in = { mix->auto_tune, {0}, mix->speaker_volume, mix->tv_volume, mix->external_volume };
    return serviceDispatchIn(&g_stplSrv, 510, in,
        .context = STPL_CONTEXT,
    );
}

//-----------------------------------------------------------------------------
// Work memory
//-----------------------------------------------------------------------------

// The slice sizes of the work memory. The reply gives the guest slices in the
// order first, third, second.
typedef struct {
    u64 common;
    u64 guest_first;
    u64 guest_third;
    u64 guest_second;
} StplWorkMemoryLayout;

static Result _stplGetWorkMemoryLayout(StplStreamingMode mode, StplWorkMemoryLayout *out) {
    const u64 type_and_mode = (u64)mode << 8;
    return serviceDispatchInOut(&g_stplSrv, 9, type_and_mode, *out,
        .context = STPL_CONTEXT,
    );
}

static Result _stplRegisterCommonWorkMemory(Handle memory, u64 size) {
    return serviceDispatchIn(&g_stplSrv, 505, size,
        .context = STPL_CONTEXT,
        .in_num_handles = 1,
        .in_handles = { memory },
    );
}

static Result _stplRegisterGuestWorkMemory(u32 guest, const u64 sizes[3], const Handle memory[3]) {
    const struct {
        u32 guest;
        u32 reserved;
        u64 sizes[3];
    } in = { guest, 0, { sizes[0], sizes[1], sizes[2] } };
    return serviceDispatchIn(&g_stplSrv, 506, in,
        .context = STPL_CONTEXT,
        .in_num_handles = 3,
        .in_handles = { memory[0], memory[1], memory[2] },
    );
}

// An empty slice is sent as a zero handle and a zero size.
static Result _stplCreateSlice(u8 **cursor, u64 size, Handle *out) {
    *out = INVALID_HANDLE;
    if (!size)
        return 0;
    Result rc = svcCreateTransferMemory(out, *cursor, size, Perm_None);
    if (R_SUCCEEDED(rc))
        *cursor += size;
    return rc;
}

static void _stplCloseSlices(Handle *slices, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (slices[i] != INVALID_HANDLE)
            svcCloseHandle(slices[i]);
    }
}

static bool _stplIsPageRange(const void *buffer, size_t size) {
    uintptr_t start = (uintptr_t)buffer;
    return buffer && size && !(start & 0xFFF) && !(size & 0xFFF) && size <= UINTPTR_MAX - start;
}

Result stplRegisterWorkMemory(void *buffer, size_t size, StplStreamingMode mode, s32 guest_count) {
    // The memory is divided between the guests below.
    if (!_stplIsPageRange(buffer, size) || guest_count < 1 || guest_count > STPL_MAX_GUESTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    StplWorkMemoryLayout layout;
    Result rc = _stplGetWorkMemoryLayout(mode, &layout);
    if (R_FAILED(rc))
        return rc;

    const u64 guest_sizes[3] = { layout.guest_first, layout.guest_second, layout.guest_third };
    const u64 guest_size = guest_sizes[0] + guest_sizes[1] + guest_sizes[2];
    if (layout.common > size || guest_size > (size - layout.common) / guest_count)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    u8 *cursor = buffer;
    Handle common;
    rc = _stplCreateSlice(&cursor, layout.common, &common);
    if (R_FAILED(rc))
        return rc;
    rc = _stplRegisterCommonWorkMemory(common, layout.common);
    _stplCloseSlices(&common, 1);

    for (s32 guest = 0; R_SUCCEEDED(rc) && guest < guest_count; guest++) {
        Handle slices[3] = { INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE };
        for (size_t i = 0; R_SUCCEEDED(rc) && i < 3; i++)
            rc = _stplCreateSlice(&cursor, guest_sizes[i], &slices[i]);
        if (R_SUCCEEDED(rc))
            rc = _stplRegisterGuestWorkMemory(guest, guest_sizes, slices);
        _stplCloseSlices(slices, 3);
    }
    return rc;
}

// The memory is back when every part of it is read-write and no longer borrowed.
static Result _stplIsWorkMemoryReturned(uintptr_t start, uintptr_t end, bool *out) {
    for (uintptr_t cursor = start; cursor < end;) {
        MemoryInfo info;
        u32 page_info;
        Result rc = svcQueryMemory(&info, &page_info, cursor);
        if (R_FAILED(rc))
            return rc;
        if ((info.attr & MemAttr_IsBorrowed) || info.perm != Perm_Rw) {
            *out = false;
            return 0;
        }
        cursor = info.addr + info.size;
    }
    *out = true;
    return 0;
}

Result stplWaitForWorkMemory(void *buffer, size_t size) {
    if (!_stplIsPageRange(buffer, size))
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const uintptr_t start = (uintptr_t)buffer;
    for (u32 attempt = 0; attempt < STPL_WORK_MEMORY_RETURN_ATTEMPTS; attempt++) {
        bool returned = false;
        Result rc = _stplIsWorkMemoryReturned(start, start + size, &returned);
        if (R_FAILED(rc) || returned)
            return rc;
        svcSleepThread(STPL_WORK_MEMORY_RETURN_POLL_NS);
    }
    return MAKERESULT(Module_Libnx, LibnxError_Timeout);
}

//-----------------------------------------------------------------------------
// Guest vibration
//-----------------------------------------------------------------------------

// A guest vibration handle packs the Npad ID (bits 0-31), the guest slot
// (32-39), the style tag's index (40-47) and the device number (48-55).
// Style tags 12 to 14 are system variants of tags 2 to 4 and use their index.
Result stplGetGuestVibrationDeviceHandles(StplVibrationDeviceHandle *handles, s32 total_handles, s32 *total_out,
                                          u8 guest, HidNpadIdType id, u32 style) {
    u32 tag = style;
    if (tag & (BIT(12) | BIT(13) | BIT(14)))
        tag >>= 10;

    s32 device_count;
    if (tag & (HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual))
        device_count = 2;
    else if (tag & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight))
        device_count = 1;
    else
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (device_count > total_handles)
        device_count = total_handles;

    const u64 base = (u64)(u32)id | (u64)guest << 32 | (u64)__builtin_ctz(tag) << 40;
    for (s32 device = 0; device < device_count; device++)
        handles[device].handle = base | (u64)device << 48;
    if (total_out) *total_out = device_count;
    return 0;
}

//-----------------------------------------------------------------------------
// Guest input
//-----------------------------------------------------------------------------

// Copies the latest entry of a ring with the sequence double-read. *out_empty
// is set when the ring has no entry yet. Returns false when the service was
// writing the entry.
static bool _stplReadLatestEntry(const u8 *ring, size_t state_size, void *out, bool *out_empty) {
    StplRingHeader *header = (StplRingHeader*)ring;
    if (!atomic_load_explicit(&header->count, memory_order_acquire)) {
        *out_empty = true;
        return true;
    }
    *out_empty = false;

    const size_t entry_size = sizeof(StplRingEntryHeader) + state_size;
    const u64 latest = atomic_load_explicit(&header->latest, memory_order_acquire) % STPL_RING_ENTRIES;
    const u8 *entry = ring + sizeof(StplRingHeader) + latest * entry_size;
    u64 *sequence = &((StplRingEntryHeader*)entry)->sequence;

    const u64 sequence0 = atomic_load_explicit(sequence, memory_order_acquire);
    if (sequence0 & 1)
        return false;
    memcpy(out, entry + sizeof(StplRingEntryHeader), state_size);
    const u64 sequence1 = atomic_load_explicit(sequence, memory_order_acquire);
    return sequence0 == sequence1;
}

static Result _stplReadSensorRing(size_t offset, size_t state_size, void *out) {
    if (!g_stplGuestInput)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    for (u32 attempt = 0; attempt < STPL_RING_READ_ATTEMPTS; attempt++) {
        bool empty;
        if (_stplReadLatestEntry(g_stplGuestInput + offset, state_size, out, &empty)) {
            if (empty)
                memset(out, 0, state_size);
            return 0;
        }
    }
    return MAKERESULT(Module_Libnx, LibnxError_Timeout);
}

static s32 _stplNpadBank(HidNpadIdType id) {
    switch (id) {
        case HidNpadIdType_No1:      return 0;
        case HidNpadIdType_No2:      return 1;
        case HidNpadIdType_Handheld: return 2;
        default:                     return -1;
    }
}

static size_t _stplGuestInputOffset(s32 bank, u8 guest) {
    return bank * STPL_NPAD_BANK_SIZE + guest * STPL_GUEST_INPUT_SIZE;
}

Result stplGetGuestNpadState(u8 guest, HidNpadIdType id, StplGuestNpadState *out) {
    // The offsets below index the guest input memory.
    const s32 bank = _stplNpadBank(id);
    if (guest >= STPL_MAX_GUESTS || bank < 0)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!g_stplGuestInput)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    // The guest's style set comes first. Its lowest standard style selects
    // the ring; a sample is valid only if the style set did not change.
    const u8 *input = g_stplGuestInput + _stplGuestInputOffset(bank, guest);
    u32 *style_set = (u32*)input;
    for (u32 attempt = 0; attempt < STPL_RING_READ_ATTEMPTS; attempt++) {
        const u32 styles = atomic_load_explicit(style_set, memory_order_acquire) & HidNpadStyleSet_NpadStandard;
        StplGuestNpadState tmp = {0};
        if (!styles) {
            *out = tmp;
            return 0;
        }

        const u32 style_index = __builtin_ctz(styles);
        const u8 *ring = input + STPL_NPAD_RINGS_OFFSET + style_index * sizeof(StplNpadRing);
        bool empty;
        if (!_stplReadLatestEntry(ring, sizeof(HidNpadCommonState), &tmp.state, &empty))
            continue;
        if (styles != (atomic_load_explicit(style_set, memory_order_acquire) & HidNpadStyleSet_NpadStandard))
            continue;
        if (empty)
            memset(&tmp.state, 0, sizeof(tmp.state));
        tmp.style = BIT(style_index);
        *out = tmp;
        return 0;
    }
    return MAKERESULT(Module_Libnx, LibnxError_Timeout);
}

Result stplGetGuestTouchScreenState(u8 guest, HidTouchScreenState *out) {
    if (guest >= STPL_MAX_GUESTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    HidTouchScreenState tmp;
    Result rc = _stplReadSensorRing(STPL_TOUCH_RINGS_OFFSET + guest * STPL_TOUCH_RING_SIZE, sizeof(tmp), &tmp);
    if (R_FAILED(rc))
        return rc;

    const s32 max_touches = sizeof(tmp.touches) / sizeof(tmp.touches[0]);
    if (tmp.count < 0) tmp.count = 0;
    if (tmp.count > max_touches) tmp.count = max_touches;
    *out = tmp;
    return 0;
}

Result stplGetGuestSixAxisSensorState(u8 guest, HidNpadIdType id, StplSixAxisSensor sensor,
                                      HidSixAxisSensorState *out) {
    const s32 bank = _stplNpadBank(id);
    if (guest >= STPL_MAX_GUESTS || bank < 0 || (u32)sensor > StplSixAxisSensor_JoyRight)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    const size_t offset = _stplGuestInputOffset(bank, guest) + STPL_SIXAXIS_RINGS_OFFSET +
                          sensor * STPL_SIXAXIS_RING_SIZE;
    return _stplReadSensorRing(offset, sizeof(*out), out);
}
