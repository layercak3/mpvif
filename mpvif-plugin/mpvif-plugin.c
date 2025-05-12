/*
 * Copyright 2025 Attila Fidan
 *
 * This file is part of mpvif-plugin.
 *
 * mpvif-plugin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * mpvif-plugin is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mpvif-plugin. If not, see <https://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

#include <mpv/client.h>

#include <wayland-client.h>
#include <wayland-util.h>

#include "ext-data-control-client-protocol.h"
#include "foreign-toplevel-management-client-protocol.h"
#include "virtual-pointer-client-protocol.h"

#define I3IPC_IMPLEMENTATION
#include "i3ipc.h"

/*
 * Using non-blocking sockets for the clipboard and properly handling multiple
 * concurrent transfers would require managing the poll array in a more complex
 * way. Just use blocking sockets and call it a day.
 */

/* UTF-8 or ambiguous text MIME types */
const char * const utf8_mimes[] = {
    "text/plain;charset=utf-8",
    "text/plain",
    "TEXT",
    "STRING",
    "UTF8_STRING"
};

/*
 * When we set the remote selection, we get an offer on the remote data device
 * for our own selection. We listen on all offers and set the mpv property to
 * the offer, which means we receive on our own offer. We're doing blocking
 * calls so this will hang the plugin. Even if that were not the case, it would
 * also unnecessarily "echo" the selection back to the host by setting the mpv
 * property and triggering mpv to set the selection again.
 *
 * A unique MIME type is offered to detect our own offers and ignore them.
 */
static char custom_mime_type_name[24];
static const char *custom_mime_type_data = "mpvif";

struct wayland_output {
    struct wl_output *obj;
    uint32_t global_id;
    struct wl_list link;
};

struct wayland_seat {
    struct wl_seat *obj;
    uint32_t global_id;
    struct wl_list link;
};

struct wayland_toplevel_handle {
    struct zwlr_foreign_toplevel_handle_v1 *obj;
    char *title;
    char *app_id;
    bool visible_on_remote_output;
    bool fullscreen;
    struct wl_list link;
};

struct wayland_data_control_source {
    struct ext_data_control_source_v1 *obj;
    char *text;
    size_t len;
};

struct mouse_pos_values {
    int64_t x;
    int64_t y;
};

static struct osd_dimensions_values {
    int64_t ml;
    int64_t mr;
    int64_t mt;
    int64_t mb;
    int64_t w;
    int64_t h;
} osd_v;

static struct video_params_values {
    int64_t w;
    int64_t h;
} video_v;

static struct wayland_toplevel_handle *current_eligible_toplevel;

static struct wayland_data_control_source selection_source;
static struct wayland_data_control_source primary_selection_source;

static struct ext_data_control_offer_v1 *dc_offer;
static int dc_offer_mime_idx = -1;
static bool dc_offer_is_our_own;

static struct wl_list wayland_output_list;
static struct wl_list wayland_seat_list;
static struct wl_list wayland_toplevel_handle_list;

static struct wl_display *display;
static struct wl_registry *registry;

static struct zwlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
static struct zwlr_virtual_pointer_v1 *virtual_pointer;

static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;

static struct ext_data_control_manager_v1 *data_control_manager;
static struct ext_data_control_device_v1 *data_control_device;

static struct wayland_output *remote_output;
static struct wayland_seat *remote_seat;

static int wakeup_pipe[2] = {-1, -1};

static uint64_t mouse_pos_reply_userdata = 1;
static uint64_t clipboard_text_reply_userdata = 2;
static uint64_t clipboard_text_primary_reply_userdata = 3;

static char *remote_display_name;
static char *remote_output_name;
static char *remote_seat_name;
static char *remote_swaysock;

static char media_title[512];

static int input_forwarding_enabled = 1;
static int force_grab_cursor_enabled = 0;

static int output_layout_x;
static int output_layout_y;

static mpv_handle *hmpv;

static bool is_eligible_toplevel(struct wayland_toplevel_handle *tl);
static void set_fullscreen_title(void);
static void set_generic_title(void);
static void destroy_toplevel_handle(struct wayland_toplevel_handle *tl);
static void logger(const char *fmt, ...);
static void destroy_data_control_source(struct wayland_data_control_source *ds);
static void destroy_data_control_device(void);
static void handle_selection(struct ext_data_control_offer_v1 *id, bool primary);
static bool should_create_virtual_pointer(void);
static void create_virtual_pointer(void);
static bool should_create_data_control_device(void);
static void create_data_control_device(void);
static void destroy_output(struct wayland_output *o);
static void destroy_seat(struct wayland_seat *s);

static void toplevel_handle_title(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1,
        const char *title)
{
    struct wayland_toplevel_handle *tl = data;
    free(tl->title);
    tl->title = strdup(title);
}

static void toplevel_handle_app_id(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1,
        const char *app_id)
{
    struct wayland_toplevel_handle *tl = data;
    free(tl->app_id);
    tl->app_id = strdup(app_id);
}

static void toplevel_handle_output_enter(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1,
        struct wl_output *output)
{
    struct wayland_toplevel_handle *tl = data;

    if (!output)
        return;

    if (wl_output_get_user_data(output) == remote_output)
        tl->visible_on_remote_output = true;
}

static void toplevel_handle_output_leave(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1,
        struct wl_output *output)
{
    struct wayland_toplevel_handle *tl = data;

    if (!output)
        return;

    if (wl_output_get_user_data(output) == remote_output)
        tl->visible_on_remote_output = false;
}

static void toplevel_handle_state(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1,
        struct wl_array *state)
{
    struct wayland_toplevel_handle *tl = data;

    tl->fullscreen = false;
    enum zwlr_foreign_toplevel_handle_v1_state *state_pos;
    wl_array_for_each(state_pos, state) {
        if (*state_pos == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) {
            tl->fullscreen = true;
            break;
        }
    }
}

static void toplevel_handle_done(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1)
{
    struct wayland_toplevel_handle *tl = data;

    if (is_eligible_toplevel(tl)) {
        if (current_eligible_toplevel != tl) {
            current_eligible_toplevel = tl;
            set_fullscreen_title();
        }
    } else {
        if (current_eligible_toplevel == tl) {
            current_eligible_toplevel = NULL;
            set_generic_title();
        }
    }
}

static void toplevel_handle_closed(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1)
{
    struct wayland_toplevel_handle *tl = data;

    destroy_toplevel_handle(tl);
}

static void toplevel_handle_parent(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1,
        struct zwlr_foreign_toplevel_handle_v1 *parent)
{
}

static const struct zwlr_foreign_toplevel_handle_v1_listener toplevel_handle_listener = {
    toplevel_handle_title,
    toplevel_handle_app_id,
    toplevel_handle_output_enter,
    toplevel_handle_output_leave,
    toplevel_handle_state,
    toplevel_handle_done,
    toplevel_handle_closed,
    toplevel_handle_parent,
};

static void toplevel_manager_toplevel(void *data,
        struct zwlr_foreign_toplevel_manager_v1 *zwlr_foreign_toplevel_manager_v1,
        struct zwlr_foreign_toplevel_handle_v1 *toplevel)
{
    struct wayland_toplevel_handle *tl = calloc(1, sizeof(*tl));
    if (!tl) {
        zwlr_foreign_toplevel_handle_v1_destroy(toplevel);
        return;
    }

    tl->obj = toplevel;
    wl_list_insert(&wayland_toplevel_handle_list, &tl->link);
    zwlr_foreign_toplevel_handle_v1_add_listener(tl->obj,
            &toplevel_handle_listener, tl);
}

static void toplevel_manager_finished(void *data,
        struct zwlr_foreign_toplevel_manager_v1 *zwlr_foreign_toplevel_manager_v1)
{
    logger("compositor is finished with our toplevel manager for some reason");
}

static const struct zwlr_foreign_toplevel_manager_v1_listener toplevel_manager_listener = {
    toplevel_manager_toplevel,
    toplevel_manager_finished,
};

static void data_control_source_send(void *data,
        struct ext_data_control_source_v1 *ext_data_control_source_v1,
        const char *mime_type, int fd)
{
    struct wayland_data_control_source *ds = data;
    const char *clip_data = NULL;
    size_t clip_len = 0;

    for (size_t i = 0; i < sizeof(utf8_mimes) / sizeof(utf8_mimes[0]); i++) {
        if (strcmp(mime_type, utf8_mimes[i]) == 0) {
            clip_data = ds->text;
            clip_len = ds->len;
            break;
        }
    }

    if (!clip_data) {
        if (strcmp(mime_type, custom_mime_type_name) == 0) {
            clip_data = custom_mime_type_data;
            clip_len = strlen(custom_mime_type_data);
        }
    }

    if (clip_data) {
        if (write(fd, clip_data, clip_len) == -1)
               logger("write() failed: %m");
    }

    close(fd);
}

static void data_control_source_cancelled(void *data,
        struct ext_data_control_source_v1 *ext_data_control_source_v1)
{
    struct wayland_data_control_source *ds = data;

    destroy_data_control_source(ds);
}

static const struct ext_data_control_source_v1_listener data_control_source_listener = {
    data_control_source_send,
    data_control_source_cancelled,
};

static void data_control_offer_offer(void *data,
        struct ext_data_control_offer_v1 *ext_data_control_offer_v1,
        const char *mime_type)
{
    if (ext_data_control_offer_v1 != dc_offer) {
        logger("unexpected data offer offer event, shouldn't happen");
        return;
    }

    if (dc_offer_is_our_own)
        return;

    if (strcmp(mime_type, custom_mime_type_name) == 0) {
        dc_offer_is_our_own = true;
        return;
    }

    /* prefer to use text/plain;charset=utf-8, applications hopefully offer this
     * one first */
    if (dc_offer_mime_idx == 0)
        return;

    for (size_t i = 0; i < sizeof(utf8_mimes) / sizeof(utf8_mimes[0]); i++) {
        if (strcmp(mime_type, utf8_mimes[i]) == 0) {
            dc_offer_mime_idx = i;
            return;
        }
    }
}

static const struct ext_data_control_offer_v1_listener data_control_offer_listener = {
    data_control_offer_offer,
};

static void data_control_device_data_offer(void *data,
        struct ext_data_control_device_v1 *ext_data_control_device_v1,
        struct ext_data_control_offer_v1 *id)
{
    if (!id)
        return;

    dc_offer = id;
    ext_data_control_offer_v1_add_listener(id, &data_control_offer_listener,
            NULL);
}

static void data_control_device_selection(void *data,
        struct ext_data_control_device_v1 *ext_data_control_device_v1,
        struct ext_data_control_offer_v1 *id)
{
    handle_selection(id, false);
}

static void data_control_device_finished(void *data,
        struct ext_data_control_device_v1 *ext_data_control_device_v1)
{
    logger("compositor is finished with our data control device for some reason");
    destroy_data_control_device();
}

static void data_control_device_primary_selection(void *data,
        struct ext_data_control_device_v1 *ext_data_control_device_v1,
        struct ext_data_control_offer_v1 *id)
{
    handle_selection(id, true);
}

static const struct ext_data_control_device_v1_listener data_control_device_listener = {
    data_control_device_data_offer,
    data_control_device_selection,
    data_control_device_finished,
    data_control_device_primary_selection,
};

static void output_geometry(void *data, struct wl_output *wl_output,
        int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
        int32_t subpixel, const char *make, const char *model,
        int32_t transform)
{
}

static void output_mode(void *data, struct wl_output *wl_output,
        uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
}

static void output_done(void *data, struct wl_output *wl_output)
{
}

static void output_scale(void *data, struct wl_output *wl_output,
        int32_t scale)
{
}

static void output_name(void *data, struct wl_output *wl_output,
        const char *name)
{
    struct wayland_output *o = data;

    if (strcmp(name, remote_output_name) == 0) {
        remote_output = o;

        if (should_create_virtual_pointer())
            create_virtual_pointer();
    }
}

static void output_description(void *data, struct wl_output *wl_output,
        const char *description)
{
}

static const struct wl_output_listener output_listener = {
    output_geometry,
    output_mode,
    output_done,
    output_scale,
    output_name,
    output_description,
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat,
        uint32_t capabilities)
{
}

static void seat_name(void *data, struct wl_seat *wl_seat,
        const char *name)
{
    struct wayland_seat *s = data;

    if (strcmp(name, remote_seat_name) == 0) {
        remote_seat = s;

        if (should_create_virtual_pointer())
            create_virtual_pointer();

        if (should_create_data_control_device())
            create_data_control_device();
    }
}

static const struct wl_seat_listener seat_listener = {
    seat_capabilities,
    seat_name,
};

static void registry_global(void *data, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version)
{
    if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        virtual_pointer_manager = wl_registry_bind(registry, name,
                &zwlr_virtual_pointer_manager_v1_interface, 2);
    }

    if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        toplevel_manager = wl_registry_bind(registry, name,
                &zwlr_foreign_toplevel_manager_v1_interface, 3);
        zwlr_foreign_toplevel_manager_v1_add_listener(toplevel_manager,
                &toplevel_manager_listener, NULL);
    }

    if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0) {
        data_control_manager = wl_registry_bind(registry, name,
                &ext_data_control_manager_v1_interface, 1);
    }

    if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wayland_output *o = calloc(1, sizeof(*o));
        if (!o)
            return;

        o->obj = wl_registry_bind(registry, name, &wl_output_interface, 4);
        o->global_id = name;
        wl_list_insert(&wayland_output_list, &o->link);
        wl_output_add_listener(o->obj, &output_listener, o);
    }

    if (strcmp(interface, wl_seat_interface.name) == 0) {
        struct wayland_seat *s = calloc(1, sizeof(*s));
        if (!s)
            return;

        s->obj = wl_registry_bind(registry, name, &wl_seat_interface, 8);
        s->global_id = name;
        wl_list_insert(&wayland_seat_list, &s->link);
        wl_seat_add_listener(s->obj, &seat_listener, s);
    }
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry,
        uint32_t name)
{
    struct wayland_output *o, *o_tmp;
    wl_list_for_each_safe(o, o_tmp, &wayland_output_list, link) {
        if (o->global_id == name) {
            destroy_output(o);
            return;
        }
    }

    struct wayland_seat *s, *s_tmp;
    wl_list_for_each_safe(s, s_tmp, &wayland_seat_list, link) {
        if (s->global_id == name) {
            destroy_seat(s);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

static void logger(const char *fmt, ...)
{
    fprintf(stderr, "mpvif-plugin: ");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int timestamp(void)
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    int ms = 1000 * tp.tv_sec + tp.tv_nsec / 1000000;
    return ms;
}

static bool str_is_set(const char *str)
{
    return str && *str != '\0';
}

struct mouse_pos_values mouse_node_get_values(mpv_node *node)
{
    struct mouse_pos_values mouse_v = {0};

    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];

        if (value->format != MPV_FORMAT_INT64)
            continue;

        if (strcmp(key, "x") == 0)
            mouse_v.x = value->u.int64;
        else if (strcmp(key, "y") == 0)
            mouse_v.y = value->u.int64;
    }

    return mouse_v;
}

void osd_node_get_values(mpv_node *node)
{
    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];

        if (value->format != MPV_FORMAT_INT64)
            continue;

        if (strcmp(key, "ml") == 0)
            osd_v.ml = value->u.int64;
        else if (strcmp(key, "mr") == 0)
            osd_v.mr = value->u.int64;
        else if (strcmp(key, "mt") == 0)
            osd_v.mt = value->u.int64;
        else if (strcmp(key, "mb") == 0)
            osd_v.mb = value->u.int64;
        else if (strcmp(key, "w") == 0)
            osd_v.w = value->u.int64;
        else if (strcmp(key, "h") == 0)
            osd_v.h = value->u.int64;
    }
}

void video_node_get_values(mpv_node *node)
{
    mpv_node_list *list = node->u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];

        if (value->format != MPV_FORMAT_INT64)
            continue;

        if (strcmp(key, "w") == 0)
            video_v.w = value->u.int64;
        else if (strcmp(key, "h") == 0)
            video_v.h = value->u.int64;
    }
}

static void set_fullscreen_title(void)
{
    snprintf(media_title, sizeof(media_title), "[%s] %s [%s %s %s]",
            current_eligible_toplevel->app_id, current_eligible_toplevel->title,
            remote_display_name, remote_output_name, remote_seat_name);
    mpv_set_property_string(hmpv, "force-media-title", media_title);
}

static void set_generic_title(void)
{
    snprintf(media_title, sizeof(media_title), "Remote desktop [%s %s %s]",
            remote_display_name, remote_output_name, remote_seat_name);
    mpv_set_property_string(hmpv, "force-media-title", media_title);
}

static void unset_title(void)
{
    mpv_set_property_string(hmpv, "force-media-title", "");
}

static bool is_eligible_toplevel(struct wayland_toplevel_handle *tl)
{
    /* FIXME: sway/wlroots bug where output_leave is sent after sending state
     * with fullscreen enum when the window is also set to floating */
    return tl->title && tl->app_id && tl->fullscreen;
}

static bool should_create_virtual_pointer(void)
{
    return !virtual_pointer && remote_output && remote_seat &&
        input_forwarding_enabled && !force_grab_cursor_enabled;
}

static void create_virtual_pointer(void)
{
    virtual_pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
                virtual_pointer_manager, remote_seat->obj, remote_output->obj);
    if (mpv_observe_property(hmpv, mouse_pos_reply_userdata, "mouse-pos", MPV_FORMAT_NODE) != 0)
        logger("failed to observe the mouse-pos property");
}

static void destroy_virtual_pointer(void)
{
    zwlr_virtual_pointer_v1_destroy(virtual_pointer);
    virtual_pointer = NULL;
    if (mpv_unobserve_property(hmpv, mouse_pos_reply_userdata) < 0)
        logger("failed to unobserve the mouse-pos property");
}

static void destroy_toplevel_handle(struct wayland_toplevel_handle *tl)
{
    if (current_eligible_toplevel == tl) {
        current_eligible_toplevel = NULL;
        set_generic_title();
    }

    zwlr_foreign_toplevel_handle_v1_destroy(tl->obj);
    free(tl->title);
    free(tl->app_id);
    wl_list_remove(&tl->link);
    free(tl);
}

static bool should_create_data_control_device(void)
{
    return !data_control_device && remote_seat && input_forwarding_enabled;
}

static void create_data_control_device(void)
{
    data_control_device = ext_data_control_manager_v1_get_data_device(
            data_control_manager, remote_seat->obj);
    ext_data_control_device_v1_add_listener(data_control_device,
            &data_control_device_listener, NULL);
    if (mpv_observe_property(hmpv, clipboard_text_reply_userdata,
                "clipboard/text", MPV_FORMAT_STRING) != 0)
        logger("failed to observe the clipboard/text property");
    if (mpv_observe_property(hmpv, clipboard_text_primary_reply_userdata,
                "clipboard/text-primary", MPV_FORMAT_STRING) != 0)
        logger("failed to observe the clipboard/text-primary property");
}

static void destroy_data_control_device(void)
{
    ext_data_control_device_v1_destroy(data_control_device);
    data_control_device = NULL;
    if (mpv_unobserve_property(hmpv, clipboard_text_reply_userdata) < 0)
        logger("failed to unobserve the clipboard/text property");
    if (mpv_unobserve_property(hmpv, clipboard_text_primary_reply_userdata) < 0)
        logger("failed to unobserve the clipboard/text-primary property");
}

static void destroy_data_control_source(struct wayland_data_control_source *ds)
{
    ext_data_control_source_v1_destroy(ds->obj);
    ds->obj = NULL;
    free(ds->text);
    ds->text = NULL;
    ds->len = 0;
}

static void destroy_dc_offer(void)
{
    ext_data_control_offer_v1_destroy(dc_offer);
    dc_offer = NULL;
    dc_offer_mime_idx = -1;
    dc_offer_is_our_own = false;
}

static void destroy_output(struct wayland_output *o)
{
    if (o == remote_output) {
        if (virtual_pointer)
            destroy_virtual_pointer();
        remote_output = NULL;
    }

    wl_output_destroy(o->obj);
    wl_list_remove(&o->link);
    free(o);
}

static void destroy_seat(struct wayland_seat *s)
{
    if (s == remote_seat) {
        if (virtual_pointer)
            destroy_virtual_pointer();
        if (data_control_device)
            destroy_data_control_device();
        remote_seat = NULL;
    }

    wl_seat_release(s->obj);
    wl_list_remove(&s->link);
    free(s);
}

static void receive_offer(bool primary)
{
    char read_buf[4096];
    FILE *mem_fp;
    char *mem_data;
    size_t mem_size;
    int receive_pipe[2];

    if (pipe2(receive_pipe, O_CLOEXEC) == -1) {
        logger("pipe2() failed: %m");
        return;
    }

    if (!(mem_fp = open_memstream(&mem_data, &mem_size))) {
        logger("open_memstream() failed: %m");
        close(receive_pipe[0]);
        close(receive_pipe[1]);
        return;
    }

    ext_data_control_offer_v1_receive(dc_offer, utf8_mimes[dc_offer_mime_idx],
            receive_pipe[1]);
    wl_display_flush(display);
    close(receive_pipe[1]);

    while (true) {
        ssize_t ret = read(receive_pipe[0], read_buf, sizeof(read_buf));
        if (ret == -1) {
            logger("read() failed: %m");
            fclose(mem_fp);
            close(receive_pipe[0]);
            free(mem_data);
            return;
        }

        if (ret == 0)
            break;

        fwrite(read_buf, ret, 1, mem_fp);
    }

    fwrite(&(char){0}, 1, 1, mem_fp);
    fclose(mem_fp);
    close(receive_pipe[0]);

    const char *prop = primary ? "clipboard/text-primary" : "clipboard/text";
    if (mem_size)
        mpv_set_property_string(hmpv, prop, mem_data);

    free(mem_data);
}

static void handle_selection(struct ext_data_control_offer_v1 *id, bool primary)
{
    if (!id) {
        if (dc_offer)
            destroy_dc_offer();
        return;
    }

    if (id != dc_offer) {
        logger("unexpected data offer offer event, shouldn't happen");
        return;
    }

    if (!dc_offer_is_our_own && dc_offer_mime_idx != -1)
        receive_offer(primary);

    destroy_dc_offer();
}

static void update_remote_selection(const char *selection_text, bool primary)
{
    if (!data_control_device)
        return;

    if (!str_is_set(selection_text))
        goto set_null;

    char *text_dup = strdup(selection_text);
    if (!text_dup)
        goto set_null;
    size_t text_len = strlen(text_dup);

    struct ext_data_control_source_v1 *data_control_source =
        ext_data_control_manager_v1_create_data_source(data_control_manager);

    ext_data_control_source_v1_offer(data_control_source, custom_mime_type_name);
    for (size_t i = 0; i < sizeof(utf8_mimes) / sizeof(utf8_mimes[0]); i++)
        ext_data_control_source_v1_offer(data_control_source, utf8_mimes[i]);

    struct wayland_data_control_source old_source;

    if (primary) {
        old_source = primary_selection_source;
        ext_data_control_source_v1_add_listener(data_control_source,
                &data_control_source_listener, &primary_selection_source);
        primary_selection_source.obj = data_control_source;
        primary_selection_source.text = text_dup;
        primary_selection_source.len = text_len;
        ext_data_control_device_v1_set_primary_selection(
                data_control_device, data_control_source);
    } else {
        old_source = selection_source;
        ext_data_control_source_v1_add_listener(data_control_source,
                &data_control_source_listener, &selection_source);
        selection_source.obj = data_control_source;
        selection_source.text = text_dup;
        selection_source.len = text_len;
        ext_data_control_device_v1_set_selection(
                data_control_device, data_control_source);
    }
    if (old_source.obj)
        destroy_data_control_source(&old_source);

    return;

set_null:
    if (primary) {
        ext_data_control_device_v1_set_primary_selection(
                data_control_device, NULL);
    } else {
        ext_data_control_device_v1_set_selection(data_control_device, NULL);
    }
}

static void pchg_mouse_pos(mpv_node *node)
{
    if (!virtual_pointer)
        return;

    struct mouse_pos_values mouse_v = mouse_node_get_values(node);

    int32_t denominator_x = osd_v.w - osd_v.ml - osd_v.mr;
    int32_t denominator_y = osd_v.h - osd_v.mt - osd_v.mb;

    if ((denominator_x == 0) || (denominator_y == 0))
        return;

    int32_t video_pos_x = (mouse_v.x - osd_v.ml) * video_v.w / denominator_x;
    int32_t video_pos_y = (mouse_v.y - osd_v.mt) * video_v.h / denominator_y;

    video_pos_x = MAX(video_pos_x, 0);
    video_pos_y = MAX(video_pos_y, 0);

    video_pos_x = MIN(video_pos_x, video_v.w);
    video_pos_y = MIN(video_pos_y, video_v.h);

    zwlr_virtual_pointer_v1_motion_absolute(virtual_pointer, timestamp(),
            video_pos_x, video_pos_y, video_v.w, video_v.h);
    zwlr_virtual_pointer_v1_frame(virtual_pointer);
}

static void pchg_clipboard_text(char **string)
{
    update_remote_selection(*string, false);
}

static void pchg_clipboard_text_primary(char **string)
{
    update_remote_selection(*string, true);
}

static void pchg_osd_dimensions(mpv_node *node)
{
    osd_node_get_values(node);
}

static void pchg_video_params(mpv_node *node)
{
    video_node_get_values(node);
}

static void pchg_wayland_remote_input_forwarding(int *value)
{
    input_forwarding_enabled = *value;
    if (!input_forwarding_enabled && virtual_pointer)
        destroy_virtual_pointer();
    if (should_create_virtual_pointer())
        create_virtual_pointer();
}

static void pchg_wayland_remote_force_grab_cursor(int *value)
{
    force_grab_cursor_enabled = *value;
    if (force_grab_cursor_enabled && virtual_pointer)
        destroy_virtual_pointer();
    if (should_create_virtual_pointer())
        create_virtual_pointer();
}

void wakeup_mpv_events(void *d)
{
    (void)write(wakeup_pipe[1], &(char){0}, 1);
}

static void property_change_event(mpv_event *event)
{
    mpv_event_property *event_prop = event->data;

    if (strcmp(event_prop->name, "mouse-pos") == 0) {
        if (event_prop->format == MPV_FORMAT_NODE)
            pchg_mouse_pos(event_prop->data);
        else
            logger("mouse-pos property unavailable/error");
    } else if (strcmp(event_prop->name, "osd-dimensions") == 0) {
        if (event_prop->format == MPV_FORMAT_NODE)
            pchg_osd_dimensions(event_prop->data);
        else
            logger("osd-dimensions property unavailable/error");
    } else if (strcmp(event_prop->name, "video-params") == 0) {
        if (event_prop->format == MPV_FORMAT_NODE)
            pchg_video_params(event_prop->data);
    } else if (strcmp(event_prop->name, "clipboard/text") == 0) {
        if (event_prop->format == MPV_FORMAT_STRING)
            pchg_clipboard_text(event_prop->data);
    } else if (strcmp(event_prop->name, "clipboard/text-primary") == 0) {
        if (event_prop->format == MPV_FORMAT_STRING)
            pchg_clipboard_text_primary(event_prop->data);
    } else if (strcmp(event_prop->name, "wayland-remote-input-forwarding") == 0) {
        if (event_prop->format == MPV_FORMAT_FLAG)
            pchg_wayland_remote_input_forwarding(event_prop->data);
        else
            logger("wayland-remote-input-forwarding property unavailable/error");
    } else if (strcmp(event_prop->name, "wayland-remote-force-grab-cursor") == 0) {
        if (event_prop->format == MPV_FORMAT_FLAG)
            pchg_wayland_remote_force_grab_cursor(event_prop->data);
        else
            logger("wayland-remote-force-grab-cursor property unavailable/error");
    }
}

static int dispatch_mpv_events(void)
{
    char drain[4096];
    (void)read(wakeup_pipe[0], drain, sizeof(drain));

    while (true) {
        mpv_event *event = mpv_wait_event(hmpv, 0);
        switch (event->event_id) {
            case MPV_EVENT_SHUTDOWN:
                return -1;
            case MPV_EVENT_NONE:
                return 0;
            case MPV_EVENT_PROPERTY_CHANGE:
                property_change_event(event);
                break;
            default:
                break;
        }
    }
}

static void update_output_layout_pos(void)
{
    I3ipc_reply_outputs *reply = i3ipc_get_outputs();
    /* During the main loop, poll would just fail. This is for the first call in
     * mpv_open_cplugin. */
    if (i3ipc_error_code() == I3IPC_ERROR_CLOSED)
        logger("sway IPC connection failed");
    if (!reply)
        return;

    for (int i = 0; i < reply->outputs_size; i++) {
        I3ipc_reply_outputs_el *output = &reply->outputs[i];

        if (strcmp(output->name, remote_output_name) == 0) {
            output_layout_x = output->rect.x;
            output_layout_y = output->rect.y;
            break;
        }
    }

    free(reply);
}

static void i3e_output(I3ipc_event *ev_any)
{
    update_output_layout_pos();
}

static void set_mpv_mouse_pos(int64_t x, int64_t y)
{
    mpv_node mouse_pos_node = {0};
    mouse_pos_node.format = MPV_FORMAT_NODE_MAP;

    mpv_node_list mouse_pos_node_list = {0};
    mouse_pos_node.u.list = &mouse_pos_node_list;
    mouse_pos_node_list.num = 3;

    char *keys[3] = {"x", "y", "hover"};
    mouse_pos_node_list.keys = keys;

    mpv_node mouse_pos_values_node[3] = {
        { .format = MPV_FORMAT_INT64, .u.int64 = x },
        { .format = MPV_FORMAT_INT64, .u.int64 = y },
        { .format = MPV_FORMAT_FLAG,  .u.flag = 1 },
    };
    mouse_pos_node_list.values = mouse_pos_values_node;

    mpv_set_property(hmpv, "mouse-pos", MPV_FORMAT_NODE, &mouse_pos_node);
}

static void i3e_cursor_warp(I3ipc_event *ev_any)
{
    I3ipc_event_cursor_warp *ev = (I3ipc_event_cursor_warp *)ev_any;

    int output_local_x = ev->lx - output_layout_x;
    int output_local_y = ev->ly - output_layout_y;

    int64_t mouse_pos_x = (output_local_x * (osd_v.w - osd_v.ml - osd_v.mr) / video_v.w) + osd_v.ml;
    int64_t mouse_pos_y = (output_local_y * (osd_v.h - osd_v.mt - osd_v.mb) / video_v.h) + osd_v.mt;

    mouse_pos_x = MAX(mouse_pos_x, 0);
    mouse_pos_x = MIN(mouse_pos_x, osd_v.w);

    mouse_pos_y = MAX(mouse_pos_y, 0);
    mouse_pos_y = MIN(mouse_pos_y, osd_v.h);

    set_mpv_mouse_pos(mouse_pos_x, mouse_pos_y);
}

static int dispatch_i3ipc_events(void)
{
    while (true) {
        I3ipc_event *ev_any = i3ipc_event_next(0);
        if (!ev_any)
            return 0;

        switch (ev_any->type) {
            case I3IPC_EVENT_SHUTDOWN:
                free(ev_any);
                return -1;
            case I3IPC_EVENT_OUTPUT:
                i3e_output(ev_any);
                break;
            case I3IPC_EVENT_CURSOR_WARP:
                i3e_cursor_warp(ev_any);
                break;
            default:
                break;
        }

        free(ev_any);
    }
}

int mpv_open_cplugin(mpv_handle *mpv)
{
    int rc = -1;
    hmpv = mpv;
    wl_list_init(&wayland_output_list);
    wl_list_init(&wayland_seat_list);
    wl_list_init(&wayland_toplevel_handle_list);

    remote_display_name = mpv_get_property_string(hmpv, "wayland-remote-display-name");
    if (!str_is_set(remote_display_name)) {
        logger("no remote display name set");
        goto done;
    }

    remote_output_name = mpv_get_property_string(hmpv, "wayland-remote-output-name");
    if (!str_is_set(remote_output_name)) {
        logger("no remote output name set");
        goto done;
    }

    remote_seat_name = mpv_get_property_string(hmpv, "wayland-remote-seat-name");
    if (!str_is_set(remote_seat_name)) {
        logger("no remote seat name set");
        goto done;
    }

    remote_swaysock = mpv_get_property_string(hmpv, "wayland-remote-swaysock");
    if (!str_is_set(remote_swaysock))
        logger("no remote swaysock set, will not relay application pointer warps to the host");

    display = wl_display_connect(remote_display_name);
    if (!display) {
        logger("failed to connect to the remote compositor");
        goto done;
    }

    registry = wl_display_get_registry(display);
    if (!registry) {
        logger("failed to get the registry object");
        goto done;
    }
    wl_registry_add_listener(registry, &registry_listener, NULL);

    wl_display_roundtrip(display);

    if (!virtual_pointer_manager) {
        logger("failed to get the required virtual pointer manager object");
        goto done;
    }

    if (!toplevel_manager)
        logger("failed to get the optional foreign toplevel manager object, force-media-title won't be updated for fullscreen windows");

    if (!data_control_manager)
        logger("failed to get the optional data control manager object, clipboard synchronization won't work");

    /* i3ipc_init_try calls free() on your string.
     * also, what if the plugin exits and is loaded again? */
    int i3ipc_event[] = {
        I3IPC_EVENT_SHUTDOWN,
        I3IPC_EVENT_OUTPUT,
        I3IPC_EVENT_CURSOR_WARP
    };
    if (str_is_set(remote_swaysock)) {
        char *remote_swaysock_dup = strdup(remote_swaysock);
        if (!remote_swaysock_dup) {
            mpv_free(remote_swaysock);
            remote_swaysock = NULL;
        } else {
            i3ipc_init_try(remote_swaysock_dup);
            i3ipc_set_nopanic(true);
            i3ipc_subscribe(i3ipc_event, sizeof(i3ipc_event) / sizeof(i3ipc_event[0]));
        }
    }

    set_generic_title();
    if (str_is_set(remote_swaysock))
        update_output_layout_pos();
    if (mpv_observe_property(hmpv, 0, "osd-dimensions",
                MPV_FORMAT_NODE) != 0) {
        logger("failed to observe the osd-dimensions property");
        goto done;
    }
    if (mpv_observe_property(hmpv, 0, "video-params",
                MPV_FORMAT_NODE) != 0) {
        logger("failed to observe the video-params property");
        goto done;
    }
    if (mpv_observe_property(hmpv, 0, "wayland-remote-input-forwarding",
                MPV_FORMAT_FLAG) != 0) {
        logger("failed to observe the wayland-remote-input-forwarding property");
        goto done;
    }
    if (mpv_observe_property(hmpv, 0, "wayland-remote-force-grab-cursor",
                MPV_FORMAT_FLAG) != 0) {
        logger("failed to observe the wayland-remote-force-grab-cursor property");
        goto done;
    }
    mpv_get_property(hmpv, "wayland-remote-input-forwarding", MPV_FORMAT_FLAG,
            &input_forwarding_enabled);
    mpv_get_property(hmpv, "wayland-remote-force-grab-cursor", MPV_FORMAT_FLAG,
            &force_grab_cursor_enabled);

    if (pipe2(wakeup_pipe, O_CLOEXEC | O_NONBLOCK) == -1) {
        logger("pipe2() failed: %m");
        goto done;
    }

    snprintf(custom_mime_type_name, sizeof(custom_mime_type_name),
            "x-mpvif-plugin-%08x", (unsigned int)arc4random());

    mpv_set_wakeup_callback(hmpv, wakeup_mpv_events, NULL);

    int i3ipc_fd = str_is_set(remote_swaysock) ? i3ipc_event_fd() : -1;
    /* seems to return 0 if i3ipc is in a failure state, which we obviously
     * don't want to add to poll */
    if (i3ipc_fd == 0)
        i3ipc_fd = -1;

    struct pollfd pfd[3] = {
        {.fd = wl_display_get_fd(display),  .events = POLLIN },
        {.fd = wakeup_pipe[0],              .events = POLLIN },
        {.fd = i3ipc_fd,                    .events = POLLIN },
    };

    while (true) {
        wl_display_flush(display);

        if (poll(pfd, 3, -1) == -1) {
            logger("poll() failed: %m");
            break;
        }

        if (pfd[0].revents & POLLIN)
            wl_display_dispatch(display);

        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            logger("error or hangup on display fd");
            break;
        }

        if (pfd[1].revents & POLLIN) {
            if (dispatch_mpv_events() == -1) {
                rc = 0;
                break;
            }
        }

        if (pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            logger("error or hangup on wakeup pipe read fd");
            break;
        }

        if (pfd[2].revents & POLLIN) {
            if (dispatch_i3ipc_events() == -1) {
                rc = 0;
                break;
            }
        }

        if (pfd[2].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            logger("error or hangup on i3ipc read fd");
            break;
        }
    }

done:
    for (int i = 0; i < 2; i++) {
        if (wakeup_pipe[i] != -1)
            close(wakeup_pipe[i]);
    }

    struct wayland_output *o, *o_tmp;
    wl_list_for_each_safe(o, o_tmp, &wayland_output_list, link)
        destroy_output(o);

    struct wayland_seat *s, *s_tmp;
    wl_list_for_each_safe(s, s_tmp, &wayland_seat_list, link)
        destroy_seat(s);

    struct wayland_toplevel_handle *tl, *tl_tmp;
    wl_list_for_each_safe(tl, tl_tmp, &wayland_toplevel_handle_list, link)
        destroy_toplevel_handle(tl);

    if (selection_source.obj)
        destroy_data_control_source(&selection_source);

    if (primary_selection_source.obj)
        destroy_data_control_source(&primary_selection_source);

    if (data_control_device)
        destroy_data_control_device();

    if (data_control_manager)
        ext_data_control_manager_v1_destroy(data_control_manager);

    if (toplevel_manager)
        zwlr_foreign_toplevel_manager_v1_stop(toplevel_manager);

    if (virtual_pointer)
        destroy_virtual_pointer();

    if (virtual_pointer_manager)
        zwlr_virtual_pointer_manager_v1_destroy(virtual_pointer_manager);

    if (display)
        wl_display_disconnect(display);

    unset_title();

    mpv_free(remote_display_name);
    mpv_free(remote_output_name);
    mpv_free(remote_seat_name);
    if (remote_swaysock)
        mpv_free(remote_swaysock);

    return rc;
}
