#define _POSIX_C_SOURCE 200809L
#include <xcb/xcb.h>
#include <xcb/present.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double monotonic(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec / 1e9;
}

/* Run on a private X server containing only the Vulkan test window. */
static xcb_window_t find_window(xcb_connection_t *connection, xcb_window_t root)
{
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
        connection, xcb_query_tree(connection, root), NULL);
    if (!tree)
        return XCB_NONE;
    xcb_window_t window = XCB_NONE;
    xcb_window_t *children = xcb_query_tree_children(tree);
    for (int i = 0; i < xcb_query_tree_children_length(tree); ++i) {
        xcb_get_window_attributes_reply_t *attributes = xcb_get_window_attributes_reply(
            connection, xcb_get_window_attributes(connection, children[i]), NULL);
        if (attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE &&
            attributes->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT) {
            if (window != XCB_NONE) {
                fputs("Expected one test window on a private X server\n", stderr);
                free(attributes);
                free(tree);
                exit(2);
            }
            window = children[i];
        }
        free(attributes);
    }
    free(tree);
    return window;
}

int main(int argc, char **argv)
{
    bool resize = argc == 2 && strcmp(argv[1], "--resize") == 0;
    if (argc != 1 && !resize)
        return 2;
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(connection))
        return 2;
    const xcb_query_extension_reply_t *extension = xcb_get_extension_data(connection, &xcb_present_id);
    if (!extension || !extension->present)
        return 2;
    xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
    xcb_window_t window = XCB_NONE;
    double deadline = monotonic() + 10;
    while (window == XCB_NONE && monotonic() < deadline) {
        window = find_window(connection, root);
        struct timespec delay = {0, 10000000};
        if (window == XCB_NONE)
            nanosleep(&delay, NULL);
    }
    if (window == XCB_NONE) {
        fputs("No test window appeared\n", stderr);
        return 1;
    }
    uint32_t eid = xcb_generate_id(connection);
    xcb_generic_error_t *error = xcb_request_check(connection,
        xcb_present_select_input_checked(connection, eid, window,
            XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY | XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY));
    if (error) {
        fprintf(stderr, "Present selection failed: X error %u\n", error->error_code);
        free(error);
        return 1;
    }
    printf("window=0x%x event=0x%x resize=%d\n", window, eid, resize);
    fflush(stdout);
    unsigned completed = 0, after_resize = 0;
    bool resized = false, failed = false;
    uint64_t previous_ust = 0, previous_msc = 0;
    deadline = monotonic() + 5;
    while (monotonic() < deadline && !failed &&
           (completed < 20 || (resize && after_resize < 10))) {
        xcb_generic_event_t *raw = xcb_poll_for_event(connection);
        if (!raw) {
            if (xcb_connection_has_error(connection)) {
                failed = true;
                break;
            }
            struct pollfd descriptor = {xcb_get_file_descriptor(connection), POLLIN, 0};
            poll(&descriptor, 1, 100);
            continue;
        }
        if (raw->response_type == 0) {
            fprintf(stderr, "X error %u during observation\n", ((xcb_generic_error_t *)raw)->error_code);
            failed = true;
        } else if ((raw->response_type & 0x7f) == XCB_GE_GENERIC) {
            xcb_ge_generic_event_t *generic = (void *)raw;
            if (generic->extension == extension->major_opcode &&
                generic->event_type == XCB_PRESENT_CONFIGURE_NOTIFY) {
                xcb_present_configure_notify_event_t *event = (void *)raw;
                if (event->event == eid && event->window == window &&
                    event->width == 480 && event->height == 360)
                    resized = true;
            }
            if (generic->extension == extension->major_opcode &&
                generic->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
                xcb_present_complete_notify_event_t *event = (void *)raw;
                if (event->event == eid && event->window == window &&
                    event->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
                    printf("serial=%u msc=%llu ust=%llu mode=%u\n", event->serial,
                           (unsigned long long)event->msc, (unsigned long long)event->ust, event->mode);
                    if (!event->ust || event->ust < previous_ust || event->msc < previous_msc)
                        failed = true;
                    previous_ust = event->ust;
                    previous_msc = event->msc;
                    ++completed;
                    if (resized)
                        ++after_resize;
                    if (resize && completed == 10) {
                        uint32_t size[] = {480, 360};
                        error = xcb_request_check(connection, xcb_configure_window_checked(connection,
                            window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size));
                        if (error) {
                            free(error);
                            failed = true;
                        }
                    }
                }
            }
        }
        free(raw);
    }
    bool passed = !failed && completed >= 20 && (!resize || after_resize >= 10);
    printf("XPRESENT_PIXMAP_%s completed=%u after_resize=%u\n", passed ? "PASS" : "FAIL",
           completed, after_resize);
    xcb_disconnect(connection);
    return passed ? 0 : 1;
}
