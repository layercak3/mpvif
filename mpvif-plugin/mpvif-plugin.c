/*
 * Copyright 2025 Attila Fidan
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mpv/client.h>

#include <wayland-client.h>
#include <wayland-util.h>

#include "foreign-toplevel-management-client-protocol.h"
#include "virtual-pointer-client-protocol.h"

static struct wl_display *display;
static struct wl_registry *registry;

static struct zwlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
static struct zwlr_virtual_pointer_v1 *virtual_pointer;

static struct zwlr_foreign_toplevel_manager_v1 *toplevel_manager;

struct wayland_output {
    struct wl_output *obj;
    uint32_t global_id;
    char *name;
    struct wl_list link;
};

struct wayland_seat {
    struct wl_seat *obj;
    uint32_t global_id;
    char *name;
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

static struct wayland_toplevel_handle *current_eligible_toplevel;

static struct wl_list wayland_output_list;
static struct wl_list wayland_seat_list;
static struct wl_list wayland_toplevel_handle_list;

static struct wayland_output *remote_output;
static struct wayland_seat *remote_seat;

static int wakeup_pipe[2] = {-1, -1};

static uint64_t mouse_pos_reply_userdata = 1;

static char *remote_display_name;
static char *remote_output_name;
static char *remote_seat_name;

static char media_title[512];

mpv_handle *hmpv;

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

static int is_eligible_toplevel(struct wayland_toplevel_handle *tl)
{
    return tl->title && tl->app_id && tl->visible_on_remote_output &&
        tl->fullscreen;
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

static void destroy_output(struct wayland_output *o)
{
    if (o == remote_output) {
        if (virtual_pointer)
            destroy_virtual_pointer();
        remote_output = NULL;
    }

    wl_output_destroy(o->obj);
    free(o->name);
    wl_list_remove(&o->link);
    free(o);
}

static void destroy_seat(struct wayland_seat *s)
{
    if (s == remote_seat) {
        if (virtual_pointer)
            destroy_virtual_pointer();
        remote_seat = NULL;
    }

    wl_seat_release(s->obj);
    free(s->name);
    wl_list_remove(&s->link);
    free(s);
}

static void destroy_toplevel_handle(struct wayland_toplevel_handle *tl)
{
    zwlr_foreign_toplevel_handle_v1_destroy(tl->obj);
    free(tl->title);
    free(tl->app_id);
    wl_list_remove(&tl->link);
    free(tl);
}

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

    struct wayland_output *o;
    wl_list_for_each(o, &wayland_output_list, link) {
        if (o->obj != output)
            continue;

        if (o == remote_output)
            tl->visible_on_remote_output = true;
    }
}

static void toplevel_handle_output_leave(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1,
        struct wl_output *output)
{
    struct wayland_toplevel_handle *tl = data;

    if (!output)
        return;

    struct wayland_output *o;
    wl_list_for_each(o, &wayland_output_list, link) {
        if (o->obj != output)
            continue;

        if (o == remote_output)
            tl->visible_on_remote_output = false;
    }
}

static void toplevel_handle_state(void *data,
        struct zwlr_foreign_toplevel_handle_v1 *zwlr_foreign_toplevel_handle_v1,
        struct wl_array *state)
{
    struct wayland_toplevel_handle *tl = data;

    tl->fullscreen = false;
    enum zwlr_foreign_toplevel_handle_v1_state *state_pos;
    wl_array_for_each(state_pos, state) {
        if (*state_pos == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN)
            tl->fullscreen = true;
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

    if (current_eligible_toplevel == tl) {
        current_eligible_toplevel = NULL;
        set_generic_title();
    }

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
    struct wayland_output *o = data;

    if (!o->name)
        return;

    if (strcmp(o->name, remote_output_name) == 0) {
        remote_output = o;

        if (virtual_pointer)
            destroy_virtual_pointer();

        if (remote_seat)
            create_virtual_pointer();
    }
}

static void output_scale(void *data, struct wl_output *wl_output,
        int32_t scale)
{
}

static void output_name(void *data, struct wl_output *wl_output,
        const char *name)
{
    struct wayland_output *o = data;
    free(o->name);
    o->name = strdup(name);
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
    free(s->name);
    s->name = strdup(name);

    if (strcmp(s->name, remote_seat_name) == 0) {
        remote_seat = s;

        if (virtual_pointer)
            destroy_virtual_pointer();

        if (remote_output)
            create_virtual_pointer();
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

static void mouse_pos_changed(mpv_node *mouse_node)
{
    if (!virtual_pointer)
        return;

    mpv_node_list *list;
    mpv_node osd_node = {0};
    mpv_node video_node = {0};

    int32_t mouse_pos_x, mouse_pos_y;
    list = mouse_node->u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];
        if (strcmp(key, "x") == 0)
            mouse_pos_x = value->u.int64;
        else if (strcmp(key, "y") == 0)
            mouse_pos_y = value->u.int64;
    }

    if (mpv_get_property(hmpv, "osd-dimensions", MPV_FORMAT_NODE, &osd_node) != 0)
        goto done;

    int32_t osd_ml, osd_mr, osd_mt, osd_mb, osd_w, osd_h;
    list = osd_node.u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];
        if (strcmp(key, "ml") == 0)
            osd_ml = value->u.int64;
        else if (strcmp(key, "mr") == 0)
            osd_mr = value->u.int64;
        else if (strcmp(key, "mt") == 0)
            osd_mt = value->u.int64;
        else if (strcmp(key, "mb") == 0)
            osd_mb = value->u.int64;
        else if (strcmp(key, "w") == 0)
            osd_w = value->u.int64;
        else if (strcmp(key, "h") == 0)
            osd_h = value->u.int64;
    }

    if (mpv_get_property(hmpv, "video-params", MPV_FORMAT_NODE, &video_node) != 0)
        goto done;

    int32_t video_w, video_h;
    list = video_node.u.list;
    for (int i = 0; i < list->num; i++) {
        char *key = list->keys[i];
        mpv_node *value = &list->values[i];
        if (strcmp(key, "w") == 0)
            video_w = value->u.int64;
        else if (strcmp(key, "h") == 0)
            video_h = value->u.int64;
    }

    int32_t video_pos_x = (mouse_pos_x - osd_ml) * video_w / (osd_w - osd_ml - osd_mr);
    int32_t video_pos_y = (mouse_pos_y - osd_mt) * video_h / (osd_h - osd_mt - osd_mb);

    if (video_pos_x < 0) video_pos_x = 0;
    if (video_pos_y < 0) video_pos_y = 0;

    zwlr_virtual_pointer_v1_motion_absolute(virtual_pointer, timestamp(),
            video_pos_x, video_pos_y, video_w, video_h);
    zwlr_virtual_pointer_v1_frame(virtual_pointer);

done:
    mpv_free_node_contents(&osd_node);
    mpv_free_node_contents(&video_node);
}

void wakeup_mpv_events(void *d)
{
    write(wakeup_pipe[1], &(char){0}, 1);
}

static int dispatch_mpv_events(void)
{
    char drain[4096];
    read(wakeup_pipe[0], drain, sizeof(drain));

    while (true) {
        mpv_event *event = mpv_wait_event(hmpv, 0);
        switch (event->event_id) {
            case MPV_EVENT_SHUTDOWN:
                return -1;
            case MPV_EVENT_NONE:
                return 0;
            case MPV_EVENT_PROPERTY_CHANGE:
                mpv_event_property *event_prop = event->data;
                if (event_prop->format == MPV_FORMAT_NODE)
                    mouse_pos_changed(event_prop->data);
                else
                    logger("mouse-pos property unavailable/error");
                break;
            default:
                break;
        }
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
    if (!remote_display_name) {
        logger("no remote display name set");
        goto done;
    }

    remote_output_name = mpv_get_property_string(hmpv, "wayland-remote-output-name");
    if (!remote_output_name) {
        logger("no remote output name set");
        goto done;
    }

    remote_seat_name = mpv_get_property_string(hmpv, "wayland-remote-seat-name");
    if (!remote_seat_name) {
        logger("no remote seat name set");
        goto done;
    }

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

    set_generic_title();

    if (pipe2(wakeup_pipe, O_CLOEXEC | O_NONBLOCK) == -1) {
        logger("pipe2() failed: %m");
        goto done;
    }

    mpv_set_wakeup_callback(hmpv, wakeup_mpv_events, NULL);

    struct pollfd pfd[2] = {
        {.fd = wl_display_get_fd(display),  .events = POLLIN },
        {.fd = wakeup_pipe[0],              .events = POLLIN },
    };

    while (true) {
        wl_display_flush(display);

        if (poll(pfd, 2, -1) == -1) {
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

    if (display)
        wl_display_disconnect(display);

    mpv_free(remote_display_name);
    mpv_free(remote_output_name);
    mpv_free(remote_seat_name);

    return rc;
}
