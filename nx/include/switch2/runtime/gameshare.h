/**
 * @file gameshare.h
 * @brief Hosting a local GameShare session (Switch 2), over \ref stplInitialize "StreamPlay".
 * @author kurumuz
 * @copyright 2026 libounce contributors
 * @license ISC
 * @note A process hosts one session at a time. The \ref stpl.h "stpl" functions can be used on the session as well.
 */
#pragma once
#if !defined(__SWITCH2__)
#error "GameShare is available only in a native Switch 2 build"
#endif

#include "../../switch/types.h"
#include "../../switch/result.h"
#include "../../switch/runtime/pad.h"
#include "../services/stpl.h"

/// Result of \ref gameshareShowMatchmaking and \ref gameshareHostCreate when the user cancels matchmaking.
#define GAMESHARE_RESULT_MATCHMAKING_CANCELLED MAKERESULT(287, 30)

/// Settings of a GameShare host session (\ref gameshareHostCreate).
typedef struct {
    s32 max_guests;                 ///< Most guests, 1 to \ref STPL_MAX_GUESTS.
    StplStreamingMode mode;         ///< \ref StplStreamingMode
    u32 style_set;                  ///< \ref HidNpadStyleTag set that guest controllers may use.
    HidNpadJoyHoldType hold_type;   ///< \ref HidNpadJoyHoldType of guest Joy-Con.
    bool sensors;                   ///< Read guest touch screens and six-axis sensors.
    s32 touch_width;                ///< Width that guest touch positions are scaled to.
    s32 touch_height;               ///< Height that guest touch positions are scaled to.
    StplAudioMix audio;             ///< Audio mix of the stream.
    AccountUid user;                ///< The host user. Zero: the last opened user, or else the user picker.
} GameShareHostConfig;

/// A GameShare host session.
typedef struct {
    s32 max_guests;                 ///< Most guests of the session.
    s32 joined;                     ///< Guests that joined in matchmaking.
    u32 session_id;                 ///< Session ID from matchmaking.
    u32 guests_cur;                 ///< Connected guests at the last \ref gameshareHostUpdate, one bit per guest slot.
    u32 guests_old;                 ///< Connected guests at the update before.
    Result connection_result;       ///< The last connection error from the service, 0 if none.
    bool stpl_initialized;          ///< Private.
    bool host_initialized;          ///< Private.
    bool matchmaking_cancelled;     ///< Private.
    bool session_open;              ///< Private.
    void* work_memory;              ///< Private: freed when the service has returned it.
    size_t work_memory_size;        ///< Private.
} GameShareHost;

/**
 * @brief Fills a \ref GameShareHostConfig with the usual settings.
 * @param[out] config Output config.
 * @param[in] max_guests Most guests, 1 to \ref STPL_MAX_GUESTS.
 * @note Mirror mode, the standard Npad styles, Joy-Con held horizontally, sensors on with
 *       a 1280x720 touch range, full volumes and the last opened user.
 */
void gameshareHostConfigDefault(GameShareHostConfig* config, s32 max_guests);

/**
 * @brief Creates a host session: sets up StreamPlay and its work memory, shows the
 *        matchmaking applet and starts streaming to the guests that joined.
 * @param[out] host Output \ref GameShareHost.
 * @param[in] config Session settings.
 * @return \ref GAMESHARE_RESULT_MATCHMAKING_CANCELLED when the user cancels matchmaking.
 * @note Call \ref gameshareHostClose afterwards whether this succeeds or fails.
 */
Result gameshareHostCreate(GameShareHost* host, const GameShareHostConfig* config);

/**
 * @brief Reads the guest connection state. Call it once per frame.
 * @param[in] host \ref GameShareHost
 * @return Result of reading the state. On failure, the connected guests stay as they were.
 */
Result gameshareHostUpdate(GameShareHost* host);

/**
 * @brief Ends the session and releases StreamPlay.
 * @param[in] host \ref GameShareHost
 * @return LibnxError_Timeout when the service has not returned the work memory: the
 *         memory is kept, and a later call tries again.
 */
Result gameshareHostClose(GameShareHost* host);

/**
 * @brief Shows the system matchmaking applet, which blocks until guests join or the user cancels.
 * @param[in] argument Input from \ref stplCreateMatchmakingArgument.
 * @param[out] out_session_id Session ID.
 * @param[out] out_joined Number of guests that joined.
 * @return \ref GAMESHARE_RESULT_MATCHMAKING_CANCELLED when the user cancels.
 * @note \ref gameshareHostCreate shows it; use this when running a session with the \ref stpl.h "stpl" functions.
 */
Result gameshareShowMatchmaking(const StplMatchmakingArgument* argument, u32* out_session_id, s32* out_joined);

/// Retrieves the connected guests, one bit per guest slot.
NX_CONSTEXPR u32 gameshareHostGetGuests(const GameShareHost* host) {
    return host->guests_cur;
}

/// Retrieves the guests that connected at the last \ref gameshareHostUpdate.
NX_CONSTEXPR u32 gameshareHostGetGuestsJoined(const GameShareHost* host) {
    return host->guests_cur & ~host->guests_old;
}

/// Retrieves the guests that disconnected at the last \ref gameshareHostUpdate.
NX_CONSTEXPR u32 gameshareHostGetGuestsLeft(const GameShareHost* host) {
    return ~host->guests_cur & host->guests_old;
}

/**
 * @brief Updates a \ref PadState from a guest's controllers, as \ref padUpdate does from local ones.
 * @param[in] pad \ref PadState, initialized with \ref padInitializeDefault, \ref padInitializeAny or
 *            \ref padInitializeWithMask.
 * @param[in] guest Guest slot.
 * @note A guest has the Npads No1, No2 and Handheld; the pad's other Npads are not read.
 *       After this, the pad.h accessors (\ref padGetButtonsDown, \ref padGetStickPos,
 *       \ref padIsConnected, ...) give the guest's input.
 */
void gameshareUpdateGuestPad(PadState* pad, u8 guest);
