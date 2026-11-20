/**
 * @file stpl.h
 * @brief Native Switch 2 StreamPlay (stpl:u): hosting a local wireless GameShare session.
 * @author TheAJLive, kurumuz
 * @copyright 2026 libounce contributors
 * @license ISC
 * @note \ref gameshare.h "gameshare.h" runs a host session with these commands in the order the service needs.
 */
#pragma once
#if !defined(__SWITCH2__)
#error "StreamPlay is available only in a native Switch 2 build"
#endif

#include "../../switch/types.h"
#include "../../switch/result.h"
#include "../../switch/kernel/event.h"
#include "../../switch/sf/service.h"
#include "../../switch/services/hid.h"
#include "../../switch/services/acc.h"

/// Most guests of one session.
#define STPL_MAX_GUESTS 3

/// Size of one guest audio frame: interleaved signed 16-bit stereo.
#define STPL_AUDIO_FRAME_SIZE 4

/// Length of a guest nickname, without its terminating NUL.
#define STPL_NICKNAME_LENGTH 32

/// StreamingMode.
typedef enum {
    StplStreamingMode_Mirror        = 0,        ///< The guests see the host's screen.
    StplStreamingMode_SeparateViews = 1,        ///< Each guest has its own display.
} StplStreamingMode;

/// Bits of the guest HID feature set (\ref stplSetGuestHidFeatures).
typedef enum {
    StplHidFeature_Unknown0 = BIT(0),           ///< Not yet identified. Part of \ref STPL_HID_FEATURES_DEFAULT.
    StplHidFeature_Sensors  = BIT(1),           ///< Guest touch screen and six-axis sensors.
    StplHidFeature_Unknown2 = BIT(2),           ///< Not yet identified. Part of \ref STPL_HID_FEATURES_DEFAULT.
    StplHidFeature_Unknown3 = BIT(3),           ///< Not yet identified. Part of \ref STPL_HID_FEATURES_DEFAULT.
} StplHidFeature;

/// The \ref StplHidFeature set a host enables for guest controllers, without the sensors.
#define STPL_HID_FEATURES_DEFAULT (StplHidFeature_Unknown0 | StplHidFeature_Unknown2 | StplHidFeature_Unknown3)

/// Guest six-axis sensors of one Npad (\ref stplGetGuestSixAxisSensorState).
typedef enum {
    StplSixAxisSensor_FullKey       = 0,        ///< Pro Controller.
    StplSixAxisSensor_Handheld      = 1,        ///< Joy-Con in handheld mode.
    StplSixAxisSensor_JoyDualLeft   = 2,        ///< Left Joy-Con of a pair.
    StplSixAxisSensor_JoyDualRight  = 3,        ///< Right Joy-Con of a pair.
    StplSixAxisSensor_JoyLeft       = 4,        ///< Left Joy-Con alone.
    StplSixAxisSensor_JoyRight      = 5,        ///< Right Joy-Con alone.
} StplSixAxisSensor;

/// Guest connection state (\ref stplGetConnectionState).
typedef struct {
    u32 guest_flags;                            ///< Bit N is set while guest slot N is connected.
    Result result;                              ///< The last connection error, 0 if none.
} StplConnectionState;

/// Audio mix of the stream (\ref stplSetAudioMix). Volumes are 0.0 to 1.0.
typedef struct {
    bool auto_tune;                             ///< Let the service adjust the volumes.
    float speaker_volume;                       ///< Console speaker volume.
    float tv_volume;                            ///< TV volume.
    float external_volume;                      ///< Headphone volume.
} StplAudioMix;

/// Latest sample of one guest Npad (\ref stplGetGuestNpadState).
typedef struct {
    u32 style;                                  ///< The guest's \ref HidNpadStyleTag, 0 when it has no standard style.
    HidNpadCommonState state;                   ///< The sample. Its attributes tell whether the Npad is connected.
} StplGuestNpadState;

/// Input data of the matchmaking applet (\ref stplCreateMatchmakingArgument).
typedef struct {
    u8 data[0x80];                              ///< Opaque.
} StplMatchmakingArgument;

/// Vibration device of one guest controller (\ref stplGetGuestVibrationDeviceHandles).
typedef struct {
    u64 handle;                                 ///< Handle
} StplVibrationDeviceHandle;

/// Initializes StreamPlay: opens a session and maps its guest input memory.
Result stplInitialize(void);

/// Exits StreamPlay.
void stplExit(void);

/// Gets the Service object of the StreamPlay session.
Service* stplGetServiceSession(void);

/**
 * @brief Gets the size of the work memory for a session.
 * @param[in] mode \ref StplStreamingMode
 * @param[in] guest_count Most guests, 1 to \ref STPL_MAX_GUESTS.
 * @param[out] out_size Size in bytes, a multiple of 0x1000.
 */
Result stplGetRequiredMemorySize(StplStreamingMode mode, s32 guest_count, u32 *out_size);

/**
 * @brief Initializes a host session.
 * @param[in] mode \ref StplStreamingMode
 * @param[in] guest_count Most guests, 1 to \ref STPL_MAX_GUESTS.
 */
Result stplInitializeHost(StplStreamingMode mode, s32 guest_count);

/// Finalizes the host session, after \ref stplCloseSession or \ref stplEndMatchmaking.
Result stplFinalizeHost(void);

/**
 * @brief Gets the event that the service signals when a guest connects or disconnects.
 * @param[out] out_event Output Event.
 * @param[in] autoclear Whether to clear the event automatically.
 */
Result stplGetConnectionEvent(Event *out_event, bool autoclear);

/// Gets the guest connection state.
Result stplGetConnectionState(StplConnectionState *out);

/// Sets the audio mix of the stream.
Result stplSetAudioMix(const StplAudioMix *mix);

/// Gets the stream type. A local session has type 0.
Result stplGetStreamType(u8 *out);

/// Gets the \ref StplStreamingMode of the host session.
Result stplGetStreamingMode(StplStreamingMode *out);

/// Sets the \ref HidNpadStyleTag set that guest controllers may use. Call before matchmaking.
Result stplSetGuestNpadStyleSet(u32 style_set);

/// Gets the \ref HidNpadStyleTag set that guest controllers may use.
Result stplGetGuestNpadStyleSet(u32 *out);

/// Sets the \ref HidNpadJoyHoldType of guest Joy-Con. Call before matchmaking.
Result stplSetGuestNpadJoyHoldType(HidNpadJoyHoldType type);

/// Gets the \ref HidNpadJoyHoldType of guest Joy-Con.
Result stplGetGuestNpadJoyHoldType(HidNpadJoyHoldType *out);

/// Sets the guest \ref StplHidFeature set. Call before matchmaking.
Result stplSetGuestHidFeatures(u32 features);

/// Gets the guest \ref StplHidFeature set.
Result stplGetGuestHidFeatures(u32 *out);

/// Sets the size that guest touch positions are scaled to. Call before matchmaking.
Result stplSetGuestTouchScreenRange(s32 width, s32 height);

/**
 * @brief Gives the service the work memory of the session.
 * @param[in] buffer Memory of the size from \ref stplGetRequiredMemorySize, 0x1000-aligned.
 * @param[in] size Its size.
 * @param[in] mode \ref StplStreamingMode of the session.
 * @param[in] guest_count Most guests of the session.
 * @note The memory is transfer memory: the application must not access it until
 *       \ref stplWaitForWorkMemory succeeds after the session is closed.
 */
Result stplRegisterWorkMemory(void *buffer, size_t size, StplStreamingMode mode, s32 guest_count);

/**
 * @brief Waits until the service has returned the work memory.
 * @param[in] buffer The memory given to \ref stplRegisterWorkMemory.
 * @param[in] size Its size.
 * @return LibnxError_Timeout if the service still holds it after one second.
 */
Result stplWaitForWorkMemory(void *buffer, size_t size);

/// Releases the guest work memory after a cancelled matchmaking.
Result stplReleaseGuestWorkMemory(void);

/**
 * @brief Creates the input data of the matchmaking applet.
 * @param[in] uid The user who hosts the session.
 * @param[in] guest_count Most guests of the session.
 * @param[out] out \ref StplMatchmakingArgument
 * @note The system matchmaking applet takes it as its input.
 */
Result stplCreateMatchmakingArgument(AccountUid uid, s32 guest_count, StplMatchmakingArgument *out);

/// Ends matchmaking after the user cancelled the matchmaking applet.
Result stplEndMatchmaking(void);

/// Gets the number of guests the stream can carry.
Result stplGetGuestCapacity(s32 *out);

/// Starts the stream of guest slot \p guest.
Result stplStartGuest(s32 guest);

/// Starts streaming, after \ref stplStartGuest for each guest slot.
Result stplStartStreaming(void);

/// Closes a session that matchmaking opened.
Result stplCloseSession(void);

/// Disconnects guest slot \p guest. The session must be streaming.
Result stplDisconnectGuest(u8 guest);

/**
 * @brief Gets the nickname of a guest.
 * @param[in] guest Guest slot.
 * @param[out] out NUL-terminated UTF-8 nickname.
 */
Result stplGetGuestNickname(u8 guest, char out[STPL_NICKNAME_LENGTH + 1]);

/**
 * @brief Gets the profile image of a guest, a JPEG.
 * @param[in] guest Guest slot.
 * @param[out] buffer Output buffer.
 * @param[in] size Its size.
 * @param[out] out_size Size of the image.
 */
Result stplGetGuestProfileImage(u8 guest, void *buffer, size_t size, size_t *out_size);

/**
 * @brief Queues audio for a guest.
 * @param[in] guest Guest slot.
 * @param[in] frames Interleaved signed 16-bit stereo frames.
 * @param[in] frame_count Number of frames.
 * @param[out] out_written Number of frames the service queued.
 */
Result stplWriteGuestAudio(u8 guest, const void *frames, u32 frame_count, u32 *out_written);

/// Gets the number of frames the audio queue of a guest holds.
Result stplGetGuestAudioCapacity(u32 *out_frames);

/// Gets the number of frames free in the audio queue of guest slot \p guest.
Result stplGetGuestAudioFreeFrames(u8 guest, u32 *out_frames);

/// Gets the number of frames queued for guest slot \p guest.
Result stplGetGuestAudioQueuedFrames(u8 guest, u32 *out_frames);

/**
 * @brief Gets the vibration devices of one guest controller.
 * @param[out] handles Output handles: two for a two-sided style, one for a single Joy-Con.
 * @param[in] total_handles Size of \p handles.
 * @param[out] total_out Number of handles written.
 * @param[in] guest Guest slot.
 * @param[in] id \ref HidNpadIdType: No1, No2 or Handheld.
 * @param[in] style One \ref HidNpadStyleTag bit.
 */
Result stplGetGuestVibrationDeviceHandles(StplVibrationDeviceHandle *handles, s32 total_handles, s32 *total_out,
                                          u8 guest, HidNpadIdType id, u32 style);

/// Sends a \ref HidVibrationValue to a guest vibration device.
Result stplSendGuestVibrationValue(StplVibrationDeviceHandle handle, const HidVibrationValue *value);

/**
 * @brief Gets the latest sample of a guest Npad.
 * @param[in] guest Guest slot.
 * @param[in] id \ref HidNpadIdType: No1, No2 or Handheld.
 * @param[out] out The sample. It is zero when the guest has no standard style or no sample yet.
 * @return LibnxError_Timeout when the service was writing the sample: read it again on the next frame.
 */
Result stplGetGuestNpadState(u8 guest, HidNpadIdType id, StplGuestNpadState *out);

/**
 * @brief Gets the latest guest touch screen sample. Needs \ref StplHidFeature_Sensors.
 * @note The number of touches is limited to the size of \ref HidTouchScreenState::touches.
 * @return LibnxError_Timeout when the service was writing the sample.
 */
Result stplGetGuestTouchScreenState(u8 guest, HidTouchScreenState *out);

/**
 * @brief Gets the latest sample of a guest six-axis sensor. Needs \ref StplHidFeature_Sensors.
 * @return LibnxError_Timeout when the service was writing the sample.
 */
Result stplGetGuestSixAxisSensorState(u8 guest, HidNpadIdType id, StplSixAxisSensor sensor,
                                      HidSixAxisSensorState *out);
