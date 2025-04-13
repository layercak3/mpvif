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

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <mpv/client.h>

#include "virtual-pointer-client-protocol.h"

struct wl_display *display;
struct wl_registry *registry;
struct zwlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
struct zwlr_virtual_pointer_v1 *virtual_pointer;

char *remote_display_name;
char *remote_output_name;
char *remote_seat_name;

mpv_handle *hmpv;

static void logger(const char *fmt, ...)
{
    fprintf(stderr, "mpvif-motion: ");

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

static void registry_global(void *data, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version)
{
    if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        virtual_pointer_manager = wl_registry_bind(registry, name,
                &zwlr_virtual_pointer_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry,
        uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void mouse_pos_changed(mpv_node *mouse_node)
{
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
    wl_display_roundtrip(display);

done:
    mpv_free_node_contents(&osd_node);
    mpv_free_node_contents(&video_node);
}

int mpv_open_cplugin(mpv_handle *mpv)
{
    int rc = -1;
    hmpv = mpv;

    /* TODO: write boilerplate to force use of the specified output/seat */

    remote_display_name = mpv_get_property_string(hmpv, "wayland-remote-display-name");
    if (!remote_display_name) {
        logger("No remote display name set.");
        goto done;
    }

    remote_output_name = mpv_get_property_string(hmpv, "wayland-remote-output-name");
    if (!remote_output_name) {
        logger("No remote output name set.");
        goto done;
    }

    remote_seat_name = mpv_get_property_string(hmpv, "wayland-remote-seat-name");
    if (!remote_seat_name) {
        logger("No remote seat name set.");
        goto done;
    }

    display = wl_display_connect(remote_display_name);
    if (!display) {
        logger("Failed to connect to the remote compositor.");
        goto done;
    }

    registry = wl_display_get_registry(display);
    if (!registry) {
        logger("Failed to get the registry object.");
        goto done;
    }
    wl_registry_add_listener(registry, &registry_listener, NULL);

    wl_display_roundtrip(display);

    if (!virtual_pointer_manager) {
        logger("Failed to get the virtual pointer manager object.");
        goto done;
    }

    virtual_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
            virtual_pointer_manager, NULL);

    if (mpv_observe_property(hmpv, 0, "mouse-pos", MPV_FORMAT_NODE) != 0)
        goto done;

    /* There's currently no need to respond to Wayland events, so the loop can
     * be simple. */
    while (true) {
        mpv_event *event = mpv_wait_event(hmpv, -1);
        switch (event->event_id) {
            case MPV_EVENT_SHUTDOWN:
                goto exit_loop;
            case MPV_EVENT_PROPERTY_CHANGE:
                mpv_event_property *event_prop = event->data;
                mouse_pos_changed(event_prop->data);
                break;
        }
    }
exit_loop: ;

    rc = 0;
done:
    mpv_free(remote_display_name);
    mpv_free(remote_output_name);
    mpv_free(remote_seat_name);
    return rc;
}
