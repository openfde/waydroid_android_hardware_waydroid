/*
 * Copyright © 2011 Benjamin Franzke
 * Copyright © 2010 Intel Corporation
 * Copyright © 2014 Collabora Ltd.
 * Copyright © 2021 Waydroid Project.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "wayland-hwc.h"
#include "egl-tools.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/memfd.h>
#include <drm_fourcc.h>
#include <system/graphics.h>
#include <syscall.h>
#include <cmath>
#include <algorithm>

#include <libsync/sw_sync.h>
#include <sync/sync.h>
#include <hardware/gralloc.h>
#include <log/log.h>
#include <thread>

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#include <cutils/properties.h>

#include <xkbcommon/xkbcommon.h>

#include <wayland-client.h>
#include <wayland-android-client-protocol.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include <pointer-gestures-unstable-v1-client-protocol.h>

using ::android::hardware::hidl_string;

const int AXIS_TOUCH_SLOT_ID = 8;
const int AXIS_TOUCH_TRACKING_ID = AXIS_TOUCH_SLOT_ID;

struct buffer;

static int find_argb_visual(struct display *display) ;
void
destroy_buffer(struct display * display ,struct buffer* buf) {
    if (buf->xcbpixmap) {
        xcb_free_pixmap(display->xcbconnection, buf->xcbpixmap);
        buf->xcbpixmap = 0;
    }
    if (buf->xpicture) {
        XRenderFreePicture(display->x11display, buf->xpicture);
        buf->xpicture = 0;
    }
    // wl_buffer_destroy(buf->buffer);
    // if (buf->isShm)
    //     munmap(buf->shm_data, buf->size);
    delete buf;
}

static int
str_starts_with(const char *a, const char *b)
{
    return strncmp(a, b, strlen(b));
}


int
get_gralloc_type(const char *gralloc)
{
    if (strcmp(gralloc, "default") == 0) {
        return GRALLOC_DEFAULT;
    } else if (strcmp(gralloc, "gbm") == 0) {
        return GRALLOC_GBM;
    } else if (strcmp(gralloc, "ranchu") == 0) {
    return GRALLOC_RANCHU;
    } else if (str_starts_with(gralloc, "minigbm_") == 0) {
        return GRALLOC_CROS;
    } else if (strcmp(gralloc, "ft2004") == 0) {
        return GRALLOC_X100;
    } else if (strcmp(gralloc, "LEOPARD") == 0) {
        return GRALLOC_LEOPARD;
    } else {
        return GRALLOC_ANDROID;
    }
}

static void
buffer_release(void *, struct wl_buffer *)
{
}

static const struct wl_buffer_listener buffer_listener = {
    buffer_release
};

int
create_android_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int format,
             int pixel_stride, buffer_handle_t target)
{
    struct android_wlegl_handle *wlegl_handle;
    struct wl_array ints;
    int *the_ints;

    buffer->width = width;
    buffer->height = height;
    buffer->format = buffer->hal_format = format;
    buffer->pixel_stride = pixel_stride;
    buffer->handle = target;

    wl_array_init(&ints);
    the_ints = (int *)wl_array_add(&ints, target->numInts * sizeof(int));
    memcpy(the_ints, target->data + target->numFds, target->numInts * sizeof(int));
    wlegl_handle = android_wlegl_create_handle(display->android_wlegl, target->numFds, &ints);
    wl_array_release(&ints);

    for (int i = 0; i < target->numFds; i++) {
        android_wlegl_handle_add_fd(wlegl_handle, target->data[i]);
    }

    buffer->buffer = android_wlegl_create_buffer(display->android_wlegl, buffer->width, buffer->height, buffer->pixel_stride, buffer->format, GRALLOC_USAGE_HW_RENDER, wlegl_handle);
    android_wlegl_handle_destroy(wlegl_handle);

    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

    return 0;
}

static void
create_succeeded(void *data,
         struct zwp_linux_buffer_params_v1 *params,
         struct wl_buffer *new_buffer)
{
    struct buffer *buffer = (struct buffer*)data;

    buffer->buffer = new_buffer;
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

    zwp_linux_buffer_params_v1_destroy(params);
}

static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
    struct buffer *buffer = (struct buffer*)data;

    buffer->buffer = NULL;

    zwp_linux_buffer_params_v1_destroy(params);

    ALOGE("%s: zwp_linux_buffer_params.create failed.", __func__);
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    create_succeeded,
    create_failed
};

bool isFormatSupported(struct display *display, uint32_t format) {
    for (int i = 0; i < display->formats_count; i++) {
        if (format == display->formats[i])
            return true;
    }
    return false;
}

int ConvertHalFormatToDrm(struct display *display, uint32_t hal_format) {
    uint32_t fmt;

    switch (hal_format) {
        case HAL_PIXEL_FORMAT_RGB_888:
            fmt = DRM_FORMAT_BGR888;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_RGB888;
            break;
        case HAL_PIXEL_FORMAT_BGRA_8888:
            fmt = DRM_FORMAT_ARGB8888;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_ABGR8888;
            break;
        case HAL_PIXEL_FORMAT_RGBX_8888:
            fmt = DRM_FORMAT_XBGR8888;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_XRGB8888;
            break;
        case HAL_PIXEL_FORMAT_RGBA_8888:
            fmt = DRM_FORMAT_ABGR8888;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_ARGB8888;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
            fmt = DRM_FORMAT_BGR565;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_RGB565;
            break;
        case HAL_PIXEL_FORMAT_YV12:
            fmt = DRM_FORMAT_YVU420;
            if (!isFormatSupported(display, fmt))
                fmt = DRM_FORMAT_GR88;
            break;
        default:
            ALOGE("Cannot convert hal format to drm format %u", hal_format);
            return -EINVAL;
    }
    if (!isFormatSupported(display, fmt)) {
        ALOGE("Current wayland display doesn't support hal format %u", hal_format);
        return -EINVAL;
    }
    return fmt;
}

int
create_dmabuf_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int hal_format, int format,
             int prime_fd, int pixel_stride, int byte_stride,
             int offset, uint64_t modifier, buffer_handle_t target)
{
    struct zwp_linux_buffer_params_v1 *params;

    assert(prime_fd >= 0);
    buffer->hal_format = hal_format;
    buffer->format = (format >= 0) ? format : ConvertHalFormatToDrm(display, hal_format);
    assert(buffer->format >= 0);
    buffer->width = width;
    buffer->height = height;
    buffer->pixel_stride = pixel_stride;
    buffer->handle = target;

    params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
    zwp_linux_buffer_params_v1_add(params, prime_fd, 0, offset, byte_stride, modifier >> 32, modifier & 0xffffffff);
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer);

    buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params, buffer->width, buffer->height, buffer->format, 0);
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

    return 0;
}

static int
ConvertHalFormatToShm(uint32_t hal_format) {
    uint32_t fmt;

    switch (hal_format) {
        case HAL_PIXEL_FORMAT_RGBX_8888:
            fmt = WL_SHM_FORMAT_XRGB8888;
            break;
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            fmt = WL_SHM_FORMAT_ARGB8888;
            break;
        default:
            ALOGE("Cannot convert hal format to shm format %u", hal_format);
            return -EINVAL;
    }
    return fmt;
}

int
create_shm_wl_buffer(struct display *display, struct buffer *buffer,
             int width, int height, int format, int pixel_stride, buffer_handle_t target)
{
    // Assume 4bpp formats or none of this is going to work
    int shm_stride = width * 4;
    int size = shm_stride * height;

    buffer->size = size;
    buffer->hal_format = format;
    buffer->format = ConvertHalFormatToShm(format);
    assert(buffer->format >= 0);
    buffer->width = width;
    buffer->height = height;
    buffer->pixel_stride = pixel_stride;
    buffer->handle = target;
    buffer->isShm = true;

    int fd = syscall(__NR_memfd_create, "buffer", MFD_ALLOW_SEALING);
    ftruncate(fd, size);
    buffer->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->shm_data == MAP_FAILED) {
        ALOGE("mmap failed");
        close(fd);

        return -1;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, size);
    buffer->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, shm_stride, buffer->format);
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
    wl_shm_pool_destroy(pool);
    close(fd);

    return 0;
}

// Call me from egl_worker_thread only!
/*void snapshot_inactive_app_window(struct display *display, struct window *window) {
    if (!window->surface || !window->last_layer_buffer
        || window->last_layer_buffer->isShm || window->snapshot_buffer) {
        // Need a surface to draw and a non-SHM buffer to make snapshot from
        return;
    }

    ALOGI("Making inactive window snapshot for %s", window->taskID.c_str());

    struct buffer *old_buf = window->last_layer_buffer;
    struct buffer *new_buf = new struct buffer();
    // FIXME won't work as expected if there are multiple surfaces
    struct wl_surface *surface = window->surface;

    int ret = create_shm_wl_buffer(display, new_buf, old_buf->width, old_buf->height,
                                    HAL_PIXEL_FORMAT_RGBA_8888, old_buf->pixel_stride, old_buf->handle);
    if (ret) {
        ALOGE("failed to create a wayland buffer for window snapshot");
        return;
    }

    egl_render_to_pixels(display, new_buf);

    wl_surface_attach(surface, new_buf->buffer, 0, 0);
    if (wl_surface_get_version(surface) >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
        wl_surface_damage_buffer(surface, 0, 0, new_buf->width, new_buf->height);
    else
        wl_surface_damage(surface, 0, 0, new_buf->width, new_buf->height);
    if (!display->viewporter && display->scale > 1) {
        // With no viewporter the scale is guaranteed to be integer
        wl_surface_set_buffer_scale(surface, (int)display->scale);
    }
    wl_surface_commit(surface);

    window->snapshot_buffer = new_buf;
}

static void
xdg_surface_handle_configure(void *, struct xdg_surface *surface,
                 uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    xdg_surface_handle_configure,
};

*/

static void
finished_computing_scale(struct display *d)
{
    char property[PROPERTY_VALUE_MAX];
    int default_density = 180;
    std::string display_scale = std::to_string(d->scale);
    property_set("waydroid.display_scale", display_scale.c_str());
    if (property_get("ro.sf.lcd_density", property, nullptr) <= 0) {
        std::string lcd_density = std::to_string(int(default_density * d->scale));
        property_set("ro.sf.lcd_density", lcd_density.c_str());
    }
}

void choose_width_height(struct display* display, int32_t hint_width, int32_t hint_height) {
    char property[PROPERTY_VALUE_MAX];
    int width = hint_width;
    int height = hint_height;

    // Ignore hint it requested
    if (property_get("persist.waydroid.width", property, nullptr) > 0) {
        display->isMaximized = false;
        width = atoi(property);
    }

    if (property_get("persist.waydroid.height", property, nullptr) > 0) {
        display->isMaximized = false;
        height = atoi(property);
    }

    display->width = width;
    display->height = height;
}

/*static void
xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *,
                              int32_t width, int32_t height,
                              struct wl_array *)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    if (width == 0 || height == 0) {
    */
		/* Compositor is deferring to us */
/*		return;
	}

    if (!display->width || !display->height) {
        choose_width_height(display, width, height);
        if (!display->isMaximized)
            xdg_toplevel_unset_maximized(window->xdg_toplevel);
    }
}

static void
send_key_event(display *data, uint32_t key, wl_keyboard_key_state state);

static void
xdg_toplevel_handle_close(void *data, struct xdg_toplevel *)
{
    struct window *window = (struct window *)data;

    // simulate user input to restart idle timeout (TODO: find a better way)
    send_key_event(window->display, 0, WL_KEYBOARD_KEY_STATE_PRESSED);
    send_key_event(window->display, 0, WL_KEYBOARD_KEY_STATE_RELEASED);

    if (window->display->task != nullptr) {
        if (window->taskID != "none") {
            if (window->taskID == "0") {
                property_set("waydroid.active_apps", "none");
                window->display->task->removeAllVisibleRecentTasks();
            } else {
                window->display->task->removeTask(stoi(window->taskID));
            }
        }
    }

    std::scoped_lock lock(window->display->windowsMutex);
    destroy_window( window, true);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    xdg_toplevel_handle_configure,
    xdg_toplevel_handle_close,
};

void
shell_surface_ping(void *, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    wl_shell_surface_pong(shell_surface, serial);
}

void
shell_surface_configure(void *data, struct wl_shell_surface *, uint32_t, int32_t width, int32_t height)
{
    struct window *window = (struct window *)data;
    struct display *display = window->display;

    if (width == 0 || height == 0) {
    */
		/* Compositor is deferring to us */
/*		return;
	}

    if (!display->width || !display->height) {
        choose_width_height(display, width, height);
    }
}

void
shell_surface_popup_done(void *, struct wl_shell_surface *)
{
}

struct wl_shell_surface_listener shell_surface_listener = {
	&shell_surface_ping,
	&shell_surface_configure,
	&shell_surface_popup_done
};
*/

void
destroy_window(struct window *window, bool keep)
{   
    // 清除X11窗口和相关缓存
    if (window->xcbwindow) {
        xcb_unmap_window(window->display->xcbconnection, window->xcbwindow);
        xcb_destroy_window(window->display->xcbconnection, window->xcbwindow);
        window->xcbwindow = 0;
    }
    if (window->backxpicture) {
        XRenderFreePicture(window->display->x11display, window->backxpicture);
        window->backxpicture = 0;
    }
    if (window->backpixmap) {
        XFreePixmap(window->display->x11display, window->backpixmap);
        window->backpixmap = 0;
    }
    if (window->xcbgc) {
        xcb_free_gc(window->display->xcbconnection, window->xcbgc);
        window->xcbgc = 0;
    }
    if (window->dri3_fd > 0) {
        close(window->dri3_fd);
        window->dri3_fd = -1;
    }
    if (window->xpicture) {
        XRenderFreePicture(window->display->x11display, window->xpicture);
        window->xpicture = 0;
    }
    xcb_flush(window->display->xcbconnection);

 /*   if (window->isActive) {
        if (window->callback)
            wl_callback_destroy(window->callback);

        for (auto it = window->surfaces.begin(); it != window->surfaces.end(); it++) {
            if (window->viewports[it->first])
                wp_viewport_destroy(window->viewports[it->first]);
            wl_subsurface_destroy(window->subsurfaces[it->first]);
            wl_surface_destroy(it->second);
        }
        if (window->xdg_toplevel)
            xdg_toplevel_destroy(window->xdg_toplevel);
        if (window->xdg_surface)
            xdg_surface_destroy(window->xdg_surface);
        if (window->shell_surface)
            wl_shell_surface_destroy(window->shell_surface);
        if (window->bg_viewport)
            wp_viewport_destroy(window->bg_viewport);
        if (window->bg_subsurface)
            wl_subsurface_destroy(window->bg_subsurface);
        if (window->bg_surface)
            wl_surface_destroy(window->bg_surface);
        if (window->bg_buffer)
            wl_buffer_destroy(window->bg_buffer);
        if (window->viewport)
            wp_viewport_destroy(window->viewport);

        wl_surface_destroy(window->surface);
        wl_display_flush(window->display->display);

        window->display->windows.erase(window->surface);
    }
    */
    if (keep)
        window->isActive = false;
    else
        delete window;
}

/*static void fractional_scale_handle_preferred_scale(void *data, struct wp_fractional_scale_v1 *,
            uint32_t scale_times_120) {
    struct display *display = (struct display *)data;
    if (!display->viewporter) {
        // We should always have the viewporter if we have the fractional scale manager
        // but for debugging purpuses we may decide to disable one
        return;
    }
    display->scale = scale_times_120 / 120.0;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred_scale
};
*/


static int
ensure_pipe(struct display* display, int input_type)
{
    if (display->input_fd[input_type] == -1) {
        display->input_fd[input_type] = open(INPUT_PIPE_NAME[input_type], O_WRONLY | O_NONBLOCK);
        if (display->input_fd[input_type] == -1) {
            ALOGE("Failed to open pipe to InputFlinger: %s", strerror(errno));
            return -1;
        }
    }
    return 0;
}

#define ADD_EVENT(type_, code_, value_)            \
    event[n].time.tv_sec = rt.tv_sec;              \
    event[n].time.tv_usec = rt.tv_nsec / 1000;     \
    event[n].type = type_;                         \
    event[n].code = code_;                         \
    event[n].value = value_;                       \
    n++;
static void
send_key_event(display *data, uint32_t key, wl_keyboard_key_state state)
{
    struct display* display = (struct display*)data;
    struct input_event event[1];
    struct timespec rt;
    unsigned int res, n = 0;

    if (key >= display->keysDown.size()) {
        ALOGE("Invalid key: %u", key);
        return;
    }

    if (ensure_pipe(display, INPUT_KEYBOARD))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }
    ADD_EVENT(EV_KEY, key, state);

    res = write(display->input_fd[INPUT_KEYBOARD], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    display->keysDown[(uint8_t)key] = state;
}

static void pointer_handle_button_to_touch_down(struct display *display) {
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, 0);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, display->ptrPrvX);
    ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, display->ptrPrvY);
    ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->isTouchDown = true;
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));

    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static void pointer_handle_button_to_touch_up(struct display *display) {
    struct input_event event[3];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    display->isTouchDown = false;
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));

    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

/*static void
pointer_handle_enter(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *surface,
                     wl_fixed_t, wl_fixed_t)
{
    struct display *display = (struct display *)data;
    display->pointer_surface = surface;
    if (display->cursor_surface){
        display->serial = serial;
        int32_t icon_hotspot_x = property_get_int32("fde.mouse_icon_hotspot_x", 5);
        int32_t icon_hotspot_y = property_get_int32("fde.mouse_icon_hotspot_y", 5);
        wl_pointer_set_cursor(pointer, serial,
                              display->cursor_surface, icon_hotspot_x, icon_hotspot_y);
    }
    //When the cursor is hidden, it will trigger a new hide request in hwcomposer's hwc_prepare.
    display->mouse_icon_addr = 0;
}

static void
pointer_handle_leave(void *data, struct wl_pointer *pointer,
                     uint32_t serial, struct wl_surface *)
{
    struct display *display = (struct display *)data;
    display->pointer_surface = NULL;
    display->mouse_icon_addr = -1;
    if (display->cursor_surface){
        wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
    }
}
*/

static void
pointer_cancel_axis_to_two_finger_touch(struct display *display){
    struct input_event event[6];
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return;

    display->axis_simulation_two_finger_started = false;
    display->gesture_scale = 260;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
       ALOGE("%s:%d error in touch clock_gettime: %s",
            __FILE__, __LINE__, strerror(errno));
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    ADD_EVENT(EV_ABS, ABS_MT_SLOT, 1);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    res = write(display->input_fd[INPUT_TOUCH], &event, sizeof(event));
    if (res < sizeof(event))
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
}

static bool
pointer_cancel_axis_to_touch(struct display *display, bool fromAxisStopEvent, bool force)
{
    display->wheelEvtIsDiscrete = false;
    // Check if the scroll wheel event has started.
    if (display->lastAxisEventNanoSeconds == 0) {
        return true;
    }

    struct input_event event[12];
    int eventSize = fromAxisStopEvent ? 3 * sizeof(input_event) : sizeof(event);
    struct timespec rt;
    unsigned int res, n = 0;

    if (ensure_pipe(display, INPUT_TOUCH))
        return false;

    if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
        ALOGE("%s:%d error in touch clock_gettime: %s",
              __FILE__, __LINE__, strerror(errno));
    }

    // Prevent minor mouse movements from interrupting the touch scrolling process while scrolling the wheel.
    if (!force && ((rt.tv_sec * 1000 * 1000 * 1000 + rt.tv_nsec - display->lastAxisEventNanoSeconds) < 300 * 1000 * 1000)) {
        return false;
    }

    display->axisY = display->ptrPrvY;
    display->lastAxisEventNanoSeconds = 0;

    if (!fromAxisStopEvent) {
        // Use the second touch click to prevent inertial scrolling
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, AXIS_TOUCH_SLOT_ID + 1);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, AXIS_TOUCH_TRACKING_ID + 1);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_X, -1);
        ADD_EVENT(EV_ABS, ABS_MT_POSITION_Y, -1);
        ADD_EVENT(EV_ABS, ABS_MT_PRESSURE, 50);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    }

    ADD_EVENT(EV_ABS, ABS_MT_SLOT, AXIS_TOUCH_SLOT_ID);
    ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
    ADD_EVENT(EV_SYN, SYN_REPORT, 0);

    if (!fromAxisStopEvent) {
        ADD_EVENT(EV_ABS, ABS_MT_SLOT, AXIS_TOUCH_SLOT_ID + 1);
        ADD_EVENT(EV_ABS, ABS_MT_TRACKING_ID, -1);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
    }

    res = write(display->input_fd[INPUT_TOUCH], &event, eventSize);
    if (res < sizeof(event)) {
        ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
        return false;
    }

    return true;
}


/*
static void
pointer_handle_motion(void *data, struct wl_pointer *,
                      uint32_t, wl_fixed_t sx, wl_fixed_t sy)
{
    if(property_get_bool("fde.x11_input", false)){
        return;
    }
    struct display* display = (struct display*)data;
    if(display->axis_simulation_two_finger_started){
        pointer_cancel_axis_to_two_finger_touch(display);
    }
    int x, y;

    if (ensure_pipe(display, INPUT_POINTER))
        return;

    if (!display->pointer_surface)
        return;
    x = wl_fixed_to_int(sx);
    y = wl_fixed_to_int(sy);
    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }
    x += display->layers[display->pointer_surface].x;
    y += display->layers[display->pointer_surface].y;

    if (display->isTouchDown) {
        display->ptrPrvX = x;
        display->ptrPrvY = y;
        pointer_handle_button_to_touch_down(display);
    } else if (pointer_cancel_axis_to_touch(display, false, false)) {
        struct input_event event[5];
        struct timespec rt;
        unsigned int res, n = 0;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                __FILE__, __LINE__, strerror(errno));
        }

        ADD_EVENT(EV_ABS, ABS_X, x);
        ADD_EVENT(EV_ABS, ABS_Y, y);
        ADD_EVENT(EV_REL, REL_X, x - display->ptrPrvX);
        ADD_EVENT(EV_REL, REL_Y, y - display->ptrPrvY);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
        display->ptrPrvX = x;
        display->ptrPrvY = y;
        bool near_bord = x <= 10 || y <= 10 || x >= display->width - 10 || y >= display->height - 10;
        if(property_get_bool("fde.inject_as_touch", false) && !near_bord){
            return;
        }

        res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));
    }
}

*/


typedef void (*KeyPressCallback)(void *data, xcb_key_press_event_t *event);
typedef void (*KeyReleaseCallback)(void *data, xcb_key_release_event_t *event);
typedef void (*ButtonPressCallback)(void *data, xcb_button_press_event_t *event);
typedef void (*ButtonReleaseCallback)(void *data, xcb_button_release_event_t *event);
typedef void (*MotionNotifyCallback)(void *data, xcb_motion_notify_event_t *event);

typedef struct {
    KeyPressCallback key_press_cb;
    KeyReleaseCallback key_release_cb;
    ButtonPressCallback button_press_cb;
    ButtonReleaseCallback button_release_cb;
    MotionNotifyCallback motion_notify_cb;
} EventDispatcher;

static EventDispatcher dispatcher = {0,0,0,0,0};
static volatile bool running = true;

void register_key_press_callback(KeyPressCallback cb) { dispatcher.key_press_cb = cb; }
void register_key_release_callback(KeyReleaseCallback cb) { dispatcher.key_release_cb = cb; }
void register_button_press_callback(ButtonPressCallback cb) { dispatcher.button_press_cb = cb; }
void register_button_release_callback(ButtonReleaseCallback cb) { dispatcher.button_release_cb = cb; }
void register_motion_notify_callback(MotionNotifyCallback cb) { dispatcher.motion_notify_cb = cb; }

void on_key_press(void *data, xcb_key_press_event_t *event) {
    ALOGE("x11 keyboard press: keycode=%u\n", event->detail);
    uint32_t key = event->detail - 8;
    if (event->detail == KEY_POWER)
        return;
    struct display* display = (struct display*)data;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL){
        display->ctrl_key_pressed = 1;
    }
    send_key_event((struct display*)data, key, (wl_keyboard_key_state)1);
}

void on_key_release(void *data, xcb_key_release_event_t *event) {
    ALOGE("x11 keyboard release: keycode=%u\n", event->detail);
    uint32_t key = event->detail - 8;
    if (key == KEY_POWER)
        return;
    struct display* display = (struct display*)data;
    if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL){
        display->ctrl_key_pressed = 0;
    }
    send_key_event((struct display*)data, key, (wl_keyboard_key_state)0);
}

void on_button_press(void *data, xcb_button_press_event_t *xcb_button_event) {
    struct display* display = (struct display*)data;
    ALOGE("display->ptrPrvX: %d, display->ptrPrvY: %d", display->ptrPrvX, display->ptrPrvY);

    ALOGE("x11 mouse press: botton=%u, position=(%d, %d)\n",
           xcb_button_event->detail, xcb_button_event->event_x, xcb_button_event->event_y);
    pointer_cancel_axis_to_touch(display, false, true);
    if(display->axis_simulation_two_finger_started){
        pointer_cancel_axis_to_two_finger_touch(display);
    }

    // Left button convert to touch event, right button reserved mouse event
    if(((xcb_button_event->detail == 1 && property_get_bool("fde.click_as_touch", false)) || display->isTouchDown) && !display->isMouseLeftDown) {
	      // convert pointer event to touch event
        pointer_handle_button_to_touch_down(display);
    }else{
        struct input_event event[2];
        struct timespec rt;
        unsigned int res, n = 0;

        if (ensure_pipe(display, INPUT_POINTER))
            return;

        if (!display->pointer_surface)
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                   __FILE__, __LINE__, strerror(errno));
        }
        if(xcb_button_event->detail == 1){
            display->isMouseLeftDown = true;
        }

        uint32_t button = 0;
        switch(xcb_button_event->detail){
            case XCB_BUTTON_INDEX_1:
                button = BTN_LEFT;
                break;
            case XCB_BUTTON_INDEX_3:
                button = BTN_RIGHT;
                break;
        }
        if(button != 0){
            ADD_EVENT(EV_KEY, button, 1);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);
            res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
        }
    }
}

void on_button_release(void *data, xcb_button_release_event_t *xcb_button_event) {
    struct display* display = (struct display*)data;
    ALOGE("display->ptrPrvX: %d, display->ptrPrvY: %d", display->ptrPrvX, display->ptrPrvY);

    ALOGE("x11 mouse release: button=%u, position=(%d, %d)\n",
           xcb_button_event->detail, xcb_button_event->event_x, xcb_button_event->event_y);
    pointer_cancel_axis_to_touch(display, false, true);
    if(display->axis_simulation_two_finger_started){
        pointer_cancel_axis_to_two_finger_touch(display);
    }

    // Left button convert to touch event, right button reserved mouse event
    if(((xcb_button_event->detail == 1 && property_get_bool("fde.click_as_touch", false)) || display->isTouchDown) && !display->isMouseLeftDown) {
        // convert pointer event to touch event
        pointer_handle_button_to_touch_up(display);
    }else{
        struct input_event event[2];
        struct timespec rt;
        unsigned int res, n = 0;

        if (ensure_pipe(display, INPUT_POINTER))
            return;

        if (!display->pointer_surface)
            return;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                   __FILE__, __LINE__, strerror(errno));
        }
        if(xcb_button_event->detail == 1){
            display->isMouseLeftDown = false;
        }

        uint32_t button = 0;
        switch(xcb_button_event->detail){
            case XCB_BUTTON_INDEX_1:
                button = BTN_LEFT;
                break;
            case XCB_BUTTON_INDEX_3:
                button = BTN_RIGHT;
                break;
        }
        if(button != 0){
            ADD_EVENT(EV_KEY, button, 0);
            ADD_EVENT(EV_SYN, SYN_REPORT, 0);
            res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
        }
    }
}

void on_motion_notify(void *data, xcb_motion_notify_event_t *event) {
    struct display* display = (struct display*)data;
    ALOGE("display->ptrPrvX: %d, display->ptrPrvY: %d", display->ptrPrvX, display->ptrPrvY);
    ALOGE("x11 mouse move: position=(%d, %d)\n",
           event->event_x, event->event_y);
    ALOGE("鼠标移动: 窗口坐标 (%d, %d) -> 屏幕坐标 (%d, %d)\n", event->event_x, event->event_y, event->root_x, event->root_y);
    if(display->axis_simulation_two_finger_started){
        pointer_cancel_axis_to_two_finger_touch(display);
    }
    int x, y;

    if (ensure_pipe(display, INPUT_POINTER)){
        ALOGE("on_motion_notify return 1------>>>>>>");
        return;
    }

    //if (!display->pointer_surface){
    //    ALOGE("on_motion_notify return 2------>>>>>>");
    //    return;
    //}

    x = event->root_x;
    y = event->root_y;
    //x = event->event_x;
    //y = event->event_y;

    if (display->scale != 1) {
        x = int(x * display->scale);
        y = int(y * display->scale);
    }
    //x += display->layers[display->pointer_surface].x;
    //y += display->layers[display->pointer_surface].y;

    if (display->isTouchDown) {
        display->ptrPrvX = x;
        display->ptrPrvY = y;
        pointer_handle_button_to_touch_down(display);
    } else if (pointer_cancel_axis_to_touch(display, false, false)) {
        struct input_event event[5];
        struct timespec rt;
        unsigned int res, n = 0;

        if (clock_gettime(CLOCK_MONOTONIC, &rt) == -1) {
            ALOGE("%s:%d error in touch clock_gettime: %s",
                __FILE__, __LINE__, strerror(errno));
        }

        ADD_EVENT(EV_ABS, ABS_X, x);
        ADD_EVENT(EV_ABS, ABS_Y, y);
        ADD_EVENT(EV_REL, REL_X, x - display->ptrPrvX);
        ADD_EVENT(EV_REL, REL_Y, y - display->ptrPrvY);
        ADD_EVENT(EV_SYN, SYN_REPORT, 0);
        display->ptrPrvX = x;
        display->ptrPrvY = y;
        bool near_bord = x <= 10 || y <= 10 || x >= display->width - 10 || y >= display->height - 10;
        if(property_get_bool("fde.inject_as_touch", false) && !near_bord){
            return;
        }

        res = write(display->input_fd[INPUT_POINTER], &event, sizeof(event));
        if (res < sizeof(event))
            ALOGE("Failed to write event for InputFlinger: %s", strerror(errno));

    }
}


void *event_loop_thread(void *arg) {
    struct display* display = (struct display*)arg;
    xcb_connection_t *connection = (xcb_connection_t *)display->xcbconnection;
    int xcb_fd = xcb_get_file_descriptor(connection);
    if (xcb_fd < 0) {
        ALOGE("Unable to obtain XCB file descriptor\n");
        return NULL;
    }

    ALOGE("enter eventloopthread");
    fd_set read_fds;
    while (running) {
        FD_ZERO(&read_fds);
        FD_SET(xcb_fd, &read_fds);

        struct timeval timeout = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms

        int ret = select(xcb_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ret < 0) {
            ALOGE("select error");
            break;
        }

        if (FD_ISSET(xcb_fd, &read_fds)) {
            while (xcb_generic_event_t *event = xcb_poll_for_event(connection)) {
                switch (event->response_type & ~0x80) {
                    case XCB_KEY_PRESS:
                        if (dispatcher.key_press_cb) {
                            dispatcher.key_press_cb(arg, (xcb_key_press_event_t *)event);
                        }
                        break;
                    case XCB_KEY_RELEASE:
                        if (dispatcher.key_release_cb) {
                            dispatcher.key_release_cb(arg, (xcb_key_release_event_t *)event);
                        }
                        break;
                    case XCB_BUTTON_PRESS:
                        if (dispatcher.button_press_cb) {
                            dispatcher.button_press_cb(arg, (xcb_button_press_event_t *)event);
                        }
                        break;
                    case XCB_BUTTON_RELEASE:
                        if (dispatcher.button_release_cb) {
                            dispatcher.button_release_cb(arg, (xcb_button_release_event_t *)event);
                        }
                        break;
                    case XCB_MOTION_NOTIFY:
                        if (dispatcher.motion_notify_cb) {
                            dispatcher.motion_notify_cb(arg, (xcb_motion_notify_event_t *)event);
                        }
                        break;
                }
                free(event);
            }
        }
    }

    return NULL;
}


struct window *
create_window(struct display *display, bool use_subsurfaces, std::string appID, std::string taskID, hwc_color_t color)
{
	ALOGE("%d %d", use_subsurfaces, color.a);
    struct window *window = new struct window();
    if (!window)
        return NULL;

    std::string appID_title;
    window->callback = NULL;
    window->display = display;
    // window->surface = wl_compositor_create_surface(display->compositor);
    window->appID = appID;
    window->taskID = taskID;
    window->isActive = true;
    window->bg_viewport = NULL;
    window->bg_buffer = NULL;
    window->bg_surface = NULL;
    window->bg_subsurface = NULL;
    window->xcbwindow = 0;

    bool calibrating = !display->height || !display->width;

    // if (display->wm_base) {
    //     window->xdg_surface =
    //             xdg_wm_base_get_xdg_surface(display->wm_base, window->surface);
    //     assert(window->xdg_surface);

    //     xdg_surface_add_listener(window->xdg_surface,
    //                                  &xdg_surface_listener, window);

    //     window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
    //     assert(window->xdg_toplevel);
    //     xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, window);
    //     if (display->isMaximized || !display->height || !display->width) {
    //         xdg_toplevel_set_fullscreen(window->xdg_toplevel,NULL);
    //         xdg_toplevel_set_maximized(window->xdg_toplevel);
	//      }   
        const hidl_string appID_hidl(appID);
        hidl_string appName_hidl(appID);
	//rename the appid to Openfde to match the desktop file name openfde.desktop 
        if (appID != "Openfde" && display->task)
            display->task->getAppName(appID_hidl, [&](const hidl_string &value)
                                      {
				       appID_title = value;
				    //   xdg_toplevel_set_title(window->xdg_toplevel, value.c_str()); 
                    });
        else{
            // xdg_toplevel_set_title(window->xdg_toplevel, appID.c_str());
	     appID_title = appID;
	}

        if (appID != "Openfde")
            appID = "openfde." + appID;
        // xdg_toplevel_set_app_id(window->xdg_toplevel, appID.c_str());
    // } else if (display->shell) {
    //     window->shell_surface =
    //         wl_shell_get_shell_surface(display->shell, window->surface);
    //     assert(window->shell_surface);

    //     wl_shell_surface_add_listener(window->shell_surface, &shell_surface_listener, window);
    //     wl_shell_surface_set_toplevel(window->shell_surface);
    //     if (display->isMaximized || !display->height || !display->width)
    //         wl_shell_surface_set_maximized(window->shell_surface, display->output);
    //     const hidl_string appID_hidl(appID);
    //     hidl_string appName_hidl(appID);
    //     if (appID != "Openfde" && display->task)
    //         display->task->getAppName(appID_hidl, [&](const hidl_string &value)
    //                                   { wl_shell_surface_set_title(window->shell_surface, value.c_str()); });
    //     else
    //         wl_shell_surface_set_title(window->shell_surface, appID.c_str());
    // } else {
    //     assert(0);
    // }

    // if (calibrating && display->fractional_scale_manager) {
    //     // We only support one global scale
    //     wp_fractional_scale_v1* fs = wp_fractional_scale_manager_v1_get_fractional_scale(
    //             display->fractional_scale_manager, window->surface);
    //     wp_fractional_scale_v1_add_listener(fs, &fractional_scale_listener, display);
    //     wl_display_roundtrip(display->display);
    //     wp_fractional_scale_v1_destroy(fs);
    // }
    finished_computing_scale(display);

    // wl_surface_commit(window->surface);

    /* Here we retrieve objects if executed without immed, or error */
    // wl_display_roundtrip(display->display);
    // wl_surface_commit(window->surface);

    if (calibrating) {
        // If we did not receive a window size from the compositor we have to fall back to using the whole output size
        // At the time of writing this happens on wlroots compositors
        if (!display->height)
            display->height = display->full_height / display->scale;
        if (!display->width)
            display->width = display->full_width / display->scale;
    }
     display->colormap = xcb_generate_id(display->xcbconnection);
        xcb_create_colormap(display->xcbconnection, XCB_COLORMAP_ALLOC_NONE, display->colormap, display->xcbscreen->root, display->visualid);

    uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
    uint32_t value_list[] = {
        0,  // 设置不透明的黑色背景，避免窗口透明
        0,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
        display->colormap
    };

     window->xcbwindow = xcb_generate_id(display->xcbconnection);
    xcb_create_window(display->xcbconnection,
                    32,
                    window->xcbwindow,
                    display->xcbscreen->root,
                    0, 0, display->width, display->height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    display->visualid,
                    value_mask, value_list);
    XRenderPictureAttributes pa;
    pa.repeat = False;
    window->xpicture = XRenderCreatePicture(display->x11display, window->xcbwindow,display->argb_format, CPRepeat, &pa);
    window->backpixmap = XCreatePixmap(display->x11display, window->xcbwindow, display->width, display->height, 32);
    if (window->backpixmap == None) {
        return NULL;
    }
    window->backxpicture = XRenderCreatePicture(display->x11display, window->backpixmap,display->argb_format, CPRepeat, &pa);

    window->xcbgc = xcb_generate_id(display->xcbconnection);
    xcb_create_gc(display->xcbconnection,window->xcbgc, window->xcbwindow, 0, NULL);
    remove_title(display->xcbconnection, window->xcbwindow);

    xcb_dri3_open_cookie_t dri3_cookie = xcb_dri3_open(display->xcbconnection, window->xcbwindow, 0);
    xcb_dri3_open_reply_t *dri3_reply = xcb_dri3_open_reply(display->xcbconnection, dri3_cookie, NULL);
    if (!dri3_reply) {
        ALOGE("Cannot open DRI3 connection");
        return NULL;
    }
    window->dri3_fd = dri3_reply->nfd > 0 ? xcb_dri3_open_reply_fds(display->xcbconnection, dri3_reply)[0] : -1;
    free(dri3_reply);
    if (window->dri3_fd < 0) {
        ALOGE("Cannot get DRI3 file descriptor");
        return NULL;
    }
    xcb_map_window(display->xcbconnection, window->xcbwindow);




    xcb_change_property(
        display->xcbconnection,
        XCB_PROP_MODE_REPLACE,
        window->xcbwindow,
        XCB_ATOM_WM_NAME,
        XCB_ATOM_STRING,
        8,
        strlen(appID_title.c_str()),
        appID_title.c_str()
    );
    ALOGE("gy xcreate xcb window %s",appID_title.c_str());

/*
    window->xcbgc = xcb_generate_id(display->xcbconnection);
    xcb_create_gc(display->xcbconnection,window->xcbgc, window->xcbwindow, 0, NULL);

    xcb_dri3_open_cookie_t dri3_cookie = xcb_dri3_open(display->xcbconnection, window->xcbwindow, 0);
    xcb_dri3_open_reply_t *dri3_reply = xcb_dri3_open_reply(display->xcbconnection, dri3_cookie, NULL);
    if (!dri3_reply) {
        ALOGE("Cannot open DRI3 connection");
    }
    window->dri3_fd = dri3_reply->nfd > 0 ? xcb_dri3_open_reply_fds(display->xcbconnection, dri3_reply)[0] : -1;
    free(dri3_reply);
    if (window->dri3_fd < 0) {
        ALOGE("Cannot get DRI3 file descriptor");
    }
    xcb_map_window(display->xcbconnection, window->xcbwindow);
    */

    // No subsurface background for us!
    // if (!use_subsurfaces && !display->subcompositor)
    //     return window;

    // int fd = syscall(SYS_memfd_create, "buffer", 0);
    // ftruncate(fd, 4);
    // void *shm_data = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    // if (shm_data == MAP_FAILED) {
    //     ALOGE("mmap failed");
    //     close(fd);
    //     exit(1);
    // }
    // uint32_t *buf = (uint32_t*)shm_data;
    // *buf = color.a << 24 | color.r << 16 | color.g << 8 | color.b;

    // struct wl_shm_pool *pool = wl_shm_create_pool(display->shm, fd, 4);
    // window->bg_buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    // wl_shm_pool_destroy(pool);
    // close(fd);

    // struct wl_surface *surface = window->surface;
    // if (!use_subsurfaces) {
    //     surface = wl_compositor_create_surface(display->compositor);
    //     struct wl_subsurface *subsurface = wl_subcompositor_get_subsurface(display->subcompositor, surface, window->surface);
    //     wl_subsurface_place_below(subsurface, window->surface);
    //     window->bg_surface = surface;
    //     window->bg_subsurface = subsurface;
    // }

    // wl_surface_attach(surface, window->bg_buffer, 0, 0);
    // wl_surface_damage_buffer(surface, 0, 0, 1, 1);

    // if (display->viewporter) {
    //     window->bg_viewport = wp_viewporter_get_viewport(display->viewporter, surface);
    //     wp_viewport_set_source(window->bg_viewport, wl_fixed_from_int(0), wl_fixed_from_int(0), wl_fixed_from_int(1), wl_fixed_from_int(1));
    //     wp_viewport_set_destination(window->bg_viewport, display->width, display->height);
    // }

    // if (display->wm_base)
    //     xdg_surface_set_window_geometry(window->xdg_surface, 0, 0, display->width, display->height);

    // struct wl_region *region = wl_compositor_create_region(display->compositor);
    // if (color.a == 0) {
    //     wl_surface_set_input_region(surface, region);
    // }
    // if (color.a == 255) {
    //     wl_region_add(region, 0, 0, display->width, display->height);
    //     wl_surface_set_opaque_region(surface, region);
    // }
    // wl_region_destroy(region);

    // wl_surface_commit(surface);

    return window;
}


struct display *
create_display(const char *gralloc)
{
    struct display *display = new struct display();
    if (display == NULL) {
        ALOGE("out of memory");
        return NULL;
    }
    // wl_log_set_handler_client(wayland_log_handler);
    display->gtype = get_gralloc_type(gralloc);
    display->refresh = 0;
    display->isMaximized = true;
    // display->display = wl_display_connect(NULL);
    // ALOGI("WAYLAND_DISPLAY: %s", getenv("WAYLAND_DISPLAY"));
    // ALOGI("XDG_RUNTIME_DIR: %s", getenv("XDG_RUNTIME_DIR"));
    // if (!display->display) {
    //     ALOGE("Couldn't open Wayland display.");
    //     return NULL;
    // }

    display->x11display = NULL;
    display->x11display = XOpenDisplay("unix:/tmp/.X11-unix/X0");
    if (!display->x11display){
        ALOGE("Couldn't connect to X11 display.");
	return NULL;
    }
    /*display->xcbconnection = xcb_connect_to_display_with_auth_info("unix:/tmp/.X11-unix/X0", NULL, NULL);
     */
	display->xcbconnection = XGetXCBConnection(display->x11display);
    if (xcb_connection_has_error(display->xcbconnection)) {
        ALOGE("Couldn't connect to X11 display.");
        xcb_disconnect(display->xcbconnection);
        delete display;
        return NULL;
    }
    display->xcbscreen = xcb_setup_roots_iterator(xcb_get_setup(display->xcbconnection)).data;
    property_set("openfde.x11.display", "1");
    sem_init(&display->egl_go, 0, 0);
    sem_init(&display->egl_done, 0, 0);

    umask(0);
    mkdir("/dev/input", S_IRWXO | S_IRWXG | S_IRWXU);
    chown("/dev/input", 1000, 1000);
    // display->registry = wl_display_get_registry(display->display);
    // wl_registry_add_listener(display->registry,
    //              &registry_listener, display);
    // wl_display_roundtrip(display->display);

    display->task = IWaydroidTask::getService();
    display->isTouchDown = false;
    display->lastAxisEventNanoSeconds = 0;
    display->gesture_scale = 260;
    display->scale = 1.0;
    display->full_width=1920;
    display->full_height=1280;
     struct display *d = (struct display*)display;
      d->input_fd[INPUT_POINTER] = -1;
        d->ptrPrvX = 0;
        d->ptrPrvY = 0;
        d->isTouchDown = false;
        d->reverseScroll = property_get_bool("persist.waydroid.reverse_scrolling", false);
        mkfifo(INPUT_PIPE_NAME[INPUT_POINTER], S_IRWXO | S_IRWXG | S_IRWXU);
        chown(INPUT_PIPE_NAME[INPUT_POINTER], 1000, 1000);
        // for emulate touch input event
        d->input_fd[INPUT_TOUCH] = -1;
        mkfifo(INPUT_PIPE_NAME[INPUT_TOUCH], S_IRWXO | S_IRWXG | S_IRWXU);
        chown(INPUT_PIPE_NAME[INPUT_TOUCH], 1000, 1000);

        d->input_fd[INPUT_KEYBOARD] = -1;
        mkfifo(INPUT_PIPE_NAME[INPUT_KEYBOARD], S_IRWXO | S_IRWXG | S_IRWXU);
        chown(INPUT_PIPE_NAME[INPUT_KEYBOARD], 1000, 1000);
     register_key_press_callback(on_key_press);
    register_key_release_callback(on_key_release);
    register_button_press_callback(on_button_press);
    register_button_release_callback(on_button_release);
    register_motion_notify_callback(on_motion_notify);
    if (! find_argb_visual(display)){
	    ALOGE("can't find argb visualid");
	    return NULL;
    }

    display->argb_format = XRenderFindStandardFormat(display->x11display, PictStandardARGB32);


    pthread_t event_thread;
    if (pthread_create(&event_thread, NULL, event_loop_thread, display) != 0) {
        ALOGE("Unable to create event processing thread\n");
    }
    return display;
}





void
destroy_display(struct display *display)
{
    // if (display->wm_base)
    //     xdg_wm_base_destroy(display->wm_base);

    // if (display->shell)
    //     wl_shell_destroy(display->shell);

    // if (display->compositor)
    //     wl_compositor_destroy(display->compositor);

    // if (display->tablet_manager) {
    //     for (struct zwp_tablet_tool_v2 *t : display->tablet_tools) {
    //         zwp_tablet_tool_v2_destroy(t);
    //     }
    //     zwp_tablet_seat_v2_destroy(display->tablet_seat);
    //     zwp_tablet_manager_v2_destroy(display->tablet_manager);
    // }

    // if (display->relative_pointer_manager)
    //     zwp_relative_pointer_manager_v1_destroy(display->relative_pointer_manager);

    // if (display->pointer_constraints)
    //     zwp_pointer_constraints_v1_destroy(display->pointer_constraints);

    // release_pointer_gestures_device(display);

    // wl_registry_destroy(display->registry);
    // wl_display_flush(display->display);
    // wl_display_disconnect(display->display);
    delete display;
}

int remove_title(xcb_connection_t *conn, xcb_window_t main_win){
          // 去掉窗口装饰（如标题栏）
    xcb_intern_atom_cookie_t hints_cookie = xcb_intern_atom(conn, 0, strlen("_MOTIF_WM_HINTS"), "_MOTIF_WM_HINTS");
    xcb_intern_atom_reply_t *hints_reply = xcb_intern_atom_reply(conn, hints_cookie, NULL);
    if (hints_reply) {
	struct {
	    uint32_t flags;
	    uint32_t functions;
	    uint32_t decorations;
	    int32_t input_mode;
	    uint32_t status;
	} motif_hints = {2, 0, 0, 0, 0}; // flags=2, decorations=0
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, main_win,
			    hints_reply->atom, hints_reply->atom, 32,
			    sizeof(motif_hints) / 4, &motif_hints);
	free(hints_reply);
    }
    return 0;
}

static int find_argb_visual(struct display *display) {
    XVisualInfo vinfo_template = { .screen = DefaultScreen(display->x11display), .depth = 32, .c_class = TrueColor };
    int n_vinfo;
    XVisualInfo *vinfo = XGetVisualInfo(display->x11display, VisualScreenMask | VisualDepthMask | VisualClassMask, &vinfo_template, &n_vinfo);

    if (!vinfo) {
        ALOGE("✗ 未找到32位深度的Visual");
        return 0;
    }

    for (int i = 0; i < n_vinfo; i++) {
        XRenderPictFormat *format = XRenderFindVisualFormat(display->x11display, vinfo[i].visual);
        if (format && format->type == PictTypeDirect && format->direct.alphaMask) {
            ALOGE("  ✓ 找到ARGB Visual: id=0x%lx\n", vinfo[i].visualid);
            display->visualid  = vinfo[i].visualid;
            XFree(vinfo);
            return 1;
        }
    }

    ALOGE("  ✗ 未找到带Alpha通道的Visual\n");
    XFree(vinfo);
    return 0;
}
