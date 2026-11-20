/* SPDX-License-Identifier: ISC
 * A display window for CPU-written images: the private backend
 * of the default NWindow and of framebuffer.c on Switch 2. See
 * cpu_window.c for the sequence and its live evidence.
 * @author kurumuz
 */
#pragma once
#include "types.h"
#include "kernel/event.h"
#include "display/native_window.h"
#include "switch2/services/gsv.h"
#include "switch2/services/vi2.h"

/// Images a window can hold, as libnx framebuffers (num_fbs 1 to 3).
#define CPUWIN_MAX_IMAGES 3

/// One RGBA8 block-linear display image in the window's backing.
typedef struct {
    u32 memory_id;          ///< GSV image memory ID.
    u32 image_id;           ///< GSV image ID.
    Vi2Dependency release;  ///< The display's release of its last present. Zero before the first present.
} CpuWindowImage;

/// A window's CPU images on its NWindow's VI2 layer. The windows share one GPU session.
typedef struct {
    bool is_open;
    NWindow* nw;             ///< The NWindow that this window presents for.
    Vi2Layer* layer;         ///< That NWindow's layer (nwindowGetVi2Layer).

    void* backing;           ///< CPU memory of all images, registered with the GPU worker.
    u64 backing_size;
    u32 range_id;            ///< Worker range ID of the backing.
    u32 width;
    u32 height;
    u32 num_images;
    CpuWindowImage images[CPUWIN_MAX_IMAGES];
} CpuWindow;

/// Opens the window for an NWindow with a VI2 layer: the shared GPU session and that layer.
Result cpuwinOpen(CpuWindow* w, NWindow* nw);
/// Closes the window. Destroy its images first. The last window closes the GPU session.
void cpuwinClose(CpuWindow* w);

/// Gets the size of one image (RGBA8, Turing 2D block-linear, 16-GOB blocks).
Result cpuwinGetImageSize(u32 width, u32 height, u64* out_size);

/**
 * @brief Registers backing memory and creates display images in it.
 * @param[in] backing 0x10000-aligned heap memory holding num_images images
 *            of \ref cpuwinGetImageSize bytes each, back to back. The window
 *            uses it until \ref cpuwinDestroyImages.
 */
Result cpuwinCreateImages(CpuWindow* w, void* backing, u64 backing_size, u32 width, u32 height, u32 num_images);
/// Unregisters the layer's image, waits for every release and destroys the images and the backing registration.
Result cpuwinDestroyImages(CpuWindow* w);

/// Waits until the display no longer reads an image.
Result cpuwinWaitImage(CpuWindow* w, u32 index);
/// Presents an image that the CPU wrote and cleaned from its data cache.
Result cpuwinPresentImage(CpuWindow* w, u32 index);

/// Byte offset of RGBA8 pixel (x, y) in an image of the given width.
u32 cpuwinPixelOffset(u32 width, u32 x, u32 y);

/// Makes a valid NWindow without a binder queue (defined in display/native_window.c).
void _nwindowCreateForCpuWindow(NWindow* nw);

/// The window of an NWindow with a VI2 layer: the default NWindow or one of
/// nwindowCreateForDisplay. It opens on first use. NULL if it cannot open.
CpuWindow* cpuwinGet(NWindow* nw);
/// Whether an NWindow's window holds images (a framebuffer exists on it).
bool cpuwinHasImages(NWindow* nw);
/// Retires the images of an NWindow's window and closes that window if it is open.
/// Returns false if the display still holds an image. Everything then stays open.
bool cpuwinCloseFor(NWindow* nw);
