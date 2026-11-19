/* SPDX-License-Identifier: ISC
 * GameShare host session over the StreamPlay client (switch2/services/stpl.h).
 *
 * gameshareHostCreate runs the session's start-up in the order the service needs,
 * and gameshareHostClose undoes the steps that ran: it closes a session that
 * matchmaking opened, or ends a cancelled matchmaking, finalizes the host,
 * releases StreamPlay and frees the work memory once the service has
 * returned it.
 */
#include <string.h>
#include "switch2/runtime/gameshare.h"
#include "services/acc.h"
#include "services/applet.h"
#include "applets/libapplet.h"
#include "applets/psel.h"
#include "../../runtime/alloc.h"

#define GAMESHARE_WORK_MEMORY_ALIGNMENT 0x1000

#define GAMESHARE_MATCHMAKING_APPLET_ID      0x01000024
#define GAMESHARE_MATCHMAKING_APPLET_VERSION 0x10000

void gameshareHostConfigDefault(GameShareHostConfig* config, s32 max_guests) {
    memset(config, 0, sizeof(*config));
    config->max_guests = max_guests;
    config->mode = StplStreamingMode_Mirror;
    config->style_set = HidNpadStyleSet_NpadStandard;
    config->hold_type = HidNpadJoyHoldType_Horizontal;
    config->sensors = true;
    config->touch_width = 1280;
    config->touch_height = 720;
    config->audio = (StplAudioMix){ false, 1.0f, 1.0f, 1.0f };
}

static Result _gameshareSelectUser(AccountUid* user) {
    if (accountUidIsValid(user))
        return 0;

    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc))
        return rc;
    rc = accountGetLastOpenedUser(user);
    accountExit();
    if (R_SUCCEEDED(rc) && accountUidIsValid(user))
        return 0;

    const PselUserSelectionSettings settings = {0};
    return pselShowUserSelector(user, &settings);
}

static Result _gameshareConfigureGuests(const GameShareHostConfig* config) {
    u32 features = STPL_HID_FEATURES_DEFAULT;
    if (config->sensors)
        features |= StplHidFeature_Sensors;

    Result rc = stplSetGuestNpadStyleSet(config->style_set);
    if (R_SUCCEEDED(rc)) rc = stplSetGuestNpadJoyHoldType(config->hold_type);
    if (R_SUCCEEDED(rc)) rc = stplSetGuestHidFeatures(features);
    if (R_SUCCEEDED(rc) && config->sensors)
        rc = stplSetGuestTouchScreenRange(config->touch_width, config->touch_height);
    if (R_SUCCEEDED(rc)) rc = stplSetAudioMix(&config->audio);
    return rc;
}

static Result _gameshareRegisterWorkMemory(GameShareHost* host, StplStreamingMode mode, u32 size) {
    host->work_memory = __libnx_aligned_alloc(GAMESHARE_WORK_MEMORY_ALIGNMENT, size);
    if (!host->work_memory)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    host->work_memory_size = size;
    return stplRegisterWorkMemory(host->work_memory, size, mode, host->max_guests);
}

static Result _gameshareStartStreaming(GameShareHost* host) {
    s32 capacity = 0;
    Result rc = stplGetGuestCapacity(&capacity);

    for (s32 guest = 0; R_SUCCEEDED(rc) && guest < host->max_guests && guest < capacity; guest++)
        rc = stplStartGuest(guest);
    if (R_SUCCEEDED(rc))
        rc = stplStartStreaming();
    return rc;
}

Result gameshareShowMatchmaking(const StplMatchmakingArgument* argument, u32* out_session_id, s32* out_joined) {
    struct {
        u32 session_id;
        s32 joined;
        u8 reserved[0xA8];
    } reply = {0};

    AppletHolder holder;
    Result rc = appletCreateLibraryApplet(&holder, (AppletId)GAMESHARE_MATCHMAKING_APPLET_ID, LibAppletMode_AllForeground);
    if (R_FAILED(rc))
        return rc;

    LibAppletArgs common;
    libappletArgsCreate(&common, GAMESHARE_MATCHMAKING_APPLET_VERSION);
    rc = libappletArgsPush(&common, &holder);
    if (R_SUCCEEDED(rc)) rc = libappletPushInData(&holder, argument, sizeof(*argument));
    if (R_SUCCEEDED(rc)) rc = appletHolderStart(&holder);
    if (R_SUCCEEDED(rc)) {
        appletHolderJoin(&holder);
        LibAppletExitReason reason = appletHolderGetExitReason(&holder);
        if (reason == LibAppletExitReason_Canceled)
            rc = GAMESHARE_RESULT_MATCHMAKING_CANCELLED;
        else if (reason != LibAppletExitReason_Normal)
            rc = MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit);
    }

    // A normal exit with no guests counts as a cancellation.
    size_t reply_size = 0;
    if (R_SUCCEEDED(rc)) rc = libappletPopOutData(&holder, &reply, sizeof(reply), &reply_size);
    if (R_SUCCEEDED(rc) && reply_size != sizeof(reply))
        rc = MAKERESULT(Module_Libnx, LibnxError_BadAppletReceiveMessage);
    if (R_SUCCEEDED(rc) && reply.joined <= 0)
        rc = GAMESHARE_RESULT_MATCHMAKING_CANCELLED;
    appletHolderClose(&holder);

    if (R_SUCCEEDED(rc)) {
        if (out_session_id) *out_session_id = reply.session_id;
        if (out_joined) *out_joined = reply.joined;
    }
    return rc;
}

Result gameshareHostCreate(GameShareHost* host, const GameShareHostConfig* config) {
    memset(host, 0, sizeof(*host));
    if (config->max_guests < 1 || config->max_guests > STPL_MAX_GUESTS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    host->max_guests = config->max_guests;

    AccountUid user = config->user;
    Result rc = _gameshareSelectUser(&user);
    if (R_FAILED(rc))
        return rc;

    rc = stplInitialize();
    if (R_FAILED(rc))
        return rc;
    host->stpl_initialized = true;

    u32 work_memory_size = 0;
    rc = stplGetRequiredMemorySize(config->mode, host->max_guests, &work_memory_size);
    if (R_FAILED(rc))
        return rc;

    rc = stplInitializeHost(config->mode, host->max_guests);
    if (R_FAILED(rc))
        return rc;
    host->host_initialized = true;

    rc = _gameshareConfigureGuests(config);
    if (R_SUCCEEDED(rc)) rc = _gameshareRegisterWorkMemory(host, config->mode, work_memory_size);
    if (R_FAILED(rc))
        return rc;

    StplMatchmakingArgument argument;
    rc = stplCreateMatchmakingArgument(user, host->max_guests, &argument);
    if (R_SUCCEEDED(rc))
        rc = gameshareShowMatchmaking(&argument, &host->session_id, &host->joined);
    if (rc == GAMESHARE_RESULT_MATCHMAKING_CANCELLED)
        host->matchmaking_cancelled = true;
    if (R_FAILED(rc))
        return rc;
    host->session_open = true;

    return _gameshareStartStreaming(host);
}

Result gameshareHostUpdate(GameShareHost* host) {
    host->guests_old = host->guests_cur;
    if (!host->session_open)
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    StplConnectionState state;
    Result rc = stplGetConnectionState(&state);
    if (R_SUCCEEDED(rc)) {
        host->guests_cur = state.guest_flags;
        host->connection_result = state.result;
    }
    return rc;
}

// Keeps the first failure of the close steps.
static void _gameshareKeepFirstError(Result* first, Result rc) {
    if (R_SUCCEEDED(*first))
        *first = rc;
}

Result gameshareHostClose(GameShareHost* host) {
    Result rc = 0;
    if (host->session_open)
        _gameshareKeepFirstError(&rc, stplCloseSession());
    if (host->matchmaking_cancelled) {
        _gameshareKeepFirstError(&rc, stplEndMatchmaking());
        _gameshareKeepFirstError(&rc, stplReleaseGuestWorkMemory());
    }
    if (host->host_initialized)
        _gameshareKeepFirstError(&rc, stplFinalizeHost());
    if (host->stpl_initialized)
        stplExit();
    host->session_open = false;
    host->matchmaking_cancelled = false;
    host->host_initialized = false;
    host->stpl_initialized = false;
    host->guests_cur = host->guests_old = 0;

    // The service holds the work memory until its session is closed.
    if (host->work_memory) {
        Result wait_rc = stplWaitForWorkMemory(host->work_memory, host->work_memory_size);
        if (R_SUCCEEDED(wait_rc)) {
            __libnx_free(host->work_memory);
            host->work_memory = NULL;
            host->work_memory_size = 0;
        }
        _gameshareKeepFirstError(&rc, wait_rc);
    }
    return rc;
}

// A guest has the Npads No1, No2 and Handheld.
static const HidNpadIdType g_gameshareGuestNpads[] = { HidNpadIdType_No1, HidNpadIdType_No2 };

static bool _gameshareIsStickMoving(const HidAnalogStickState* stick) {
    return stick->x != 0 || stick->y != 0;
}

// Adds one guest Npad's sample to the pad, as padUpdate does for a local Npad.
static void _gameshareAddNpadState(PadState* pad, const StplGuestNpadState* npad) {
    pad->style_set   |= npad->style;
    pad->attributes  |= npad->state.attributes;
    pad->buttons_cur |= npad->state.buttons;
    if (!_gameshareIsStickMoving(&pad->sticks[0]))
        pad->sticks[0] = npad->state.analog_stick_l;
    if (!_gameshareIsStickMoving(&pad->sticks[1]))
        pad->sticks[1] = npad->state.analog_stick_r;
}

static bool _gameshareReadNpad(u8 guest, HidNpadIdType id, StplGuestNpadState* out) {
    return R_SUCCEEDED(stplGetGuestNpadState(guest, id, out)) && out->style &&
           (out->state.attributes & HidNpadAttribute_IsConnected);
}

void gameshareUpdateGuestPad(PadState* pad, u8 guest) {
    pad->active_id_mask = 0;
    pad->active_handheld = false;
    pad->style_set = 0;
    pad->attributes = 0;
    pad->buttons_old = pad->buttons_cur;
    pad->buttons_cur = 0;
    pad->sticks[0] = pad->sticks[1] = (HidAnalogStickState){ 0, 0 };
    pad->gc_triggers[0] = pad->gc_triggers[1] = 0;

    StplGuestNpadState npad;
    if (pad->read_handheld && _gameshareReadNpad(guest, HidNpadIdType_Handheld, &npad)) {
        pad->active_handheld = true;
        _gameshareAddNpadState(pad, &npad);
    }
    for (size_t i = 0; i < sizeof(g_gameshareGuestNpads) / sizeof(g_gameshareGuestNpads[0]); i++) {
        const u32 index = g_gameshareGuestNpads[i] - HidNpadIdType_No1;
        if ((pad->id_mask & BIT(index)) && _gameshareReadNpad(guest, g_gameshareGuestNpads[i], &npad)) {
            pad->active_id_mask |= BIT(index);
            _gameshareAddNpadState(pad, &npad);
        }
    }
}
