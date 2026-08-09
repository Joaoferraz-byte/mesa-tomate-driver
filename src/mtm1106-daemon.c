/*
 * mtm1106-daemon.c -- userspace driver for the MTM-1106 / T501 (08f2:6811).
 *
 * Why this exists (criteria-driven analysis, see Discoveries.md):
 *   - The tablet exposes two HID interfaces: interface 1 carries the pen
 *     events, interface 2 the hotkey keyboard. In the factory (mobile)
 *     mode interface 1 emits 8-byte "mouse-like" reports that restrict
 *     input to the small Android area; after the mode-switch report the
 *     firmware emits 64-byte reports covering the full PC area.
 *   - The kernel hid-generic driver parses the descriptor at first
 *     enumeration and keeps the 8-byte device forever: the one-shot
 *     activator (mtm1106-mode) cannot reliably make the kernel reload
 *     the new descriptor (reset races, address changes, reprobe on the
 *     wrong bus/address).
 *   - The mx002 and vin1060plus userspace drivers work precisely because
 *     they bypass the kernel: they read the 64-byte interrupt endpoint
 *     directly and inject events through uinput.
 *
 * This daemon therefore: (1) claims all interfaces (kernel drivers get
 * detached), (2) sends the digimend mode-switch sequence once, (3) reads
 * the 64-byte interrupt endpoint in a loop and emits ABS_X/Y/PRESSURE,
 * BTN_TOOL_PEN, BTN_TOUCH and the tablet hotkeys via uinput. On device
 * disconnect it re-waits for the tablet and repeats the handshake, so
 * replugging just works.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libusb-1.0/libusb.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MTM_VENDOR_ID 0x08f2u
#define MTM_PRODUCT_ID 0x6811u
#define MTM_REPORT_TYPE_AND_ID 0x0308u
#define MTM_REQUEST_TYPE 0x21u
#define MTM_SET_REPORT 0x09u
#define MTM_REPORT_LENGTH 8u
#define MTM_CONTROL_TRANSFER_MS 250u
#define MTM_EVENT_TRANSFER_MS 3000u
#define MTM_RECONNECT_SLEEP_MS 500u
#define MTM_RECONNECT_ATTEMPTS 200

static const uint8_t digimend_reports[][MTM_REPORT_LENGTH] = {
    {0x08, 0x04, 0x1d, 0x01, 0xff, 0xff, 0x06, 0x2e},
    {0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0},
    {0x08, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0},
};
#define DIGIMEND_REPORT_COUNT (sizeof(digimend_reports) / sizeof(digimend_reports[0]))

/*
 * Hotkey mapping (same as the mx002 reference driver). The 64-byte report
 * packs the pressed hotkey id in bytes 11/12 as two's-complement masks.
 */
static const uint8_t hotkey_keycodes[12] = {
    KEY_TAB, KEY_SPACE, KEY_LEFTALT, KEY_LEFTCTRL,
    KEY_SCROLLDOWN, KEY_SCROLLUP,
    KEY_LEFTBRACE, KEY_KPMINUS, KEY_KPPLUS,
    KEY_E, 0, KEY_B,
};

static volatile sig_atomic_t running = 1;

static void signal_handler(int signum)
{
    (void)signum;
    running = 0;
}

static libusb_device *find_tablet(libusb_context *context)
{
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0)
        return NULL;

    libusb_device *selected = NULL;
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor descriptor;
        libusb_device *candidate = list[i];
        if (libusb_get_device_descriptor(candidate, &descriptor) != 0)
            continue;
        if (descriptor.idVendor == MTM_VENDOR_ID &&
            descriptor.idProduct == MTM_PRODUCT_ID) {
            selected = candidate;
            libusb_ref_device(selected);
            break;
        }
    }
    libusb_free_device_list(list, 1);
    return selected;
}

static int wait_for_tablet(libusb_context *context, libusb_device **device_out)
{
    for (int attempt = 0; attempt < MTM_RECONNECT_ATTEMPTS && running; ++attempt) {
        *device_out = find_tablet(context);
        if (*device_out != NULL) {
            if (attempt > 0)
                printf("Tablet reappeared on bus %u, address %u.\n",
                       libusb_get_bus_number(*device_out),
                       libusb_get_device_address(*device_out));
            return 0;
        }
        usleep(MTM_RECONNECT_SLEEP_MS * 1000);
    }
    return -1;
}

/*
 * Claim every interface (interfaces 0/1/2: CDROM storage, pen, hotkeys) so
 * the kernel hid drivers stay out while the daemon owns the device.
 */
static int claim_all(libusb_device_handle *handle, int num_interfaces,
                     bool *claimed_out)
{
    int claimed = 0;
    for (int i = 0; i < num_interfaces; ++i) {
        int rc = libusb_claim_interface(handle, i);
        if (rc == 0) {
            claimed_out[i] = true;
            ++claimed;
        } else if (rc != LIBUSB_ERROR_BUSY && rc != LIBUSB_ERROR_NOT_FOUND) {
            fprintf(stderr, "Warning: could not claim interface %d: %s\n",
                    i, libusb_error_name(rc));
        }
    }
    return claimed;
}

static void release_all(libusb_device_handle *handle, const bool *claimed,
                        int num_interfaces)
{
    for (int i = 0; i < num_interfaces; ++i) {
        if (claimed[i]) {
            libusb_release_interface(handle, i);
            /* Kernel drivers take the device back; udev will reload them. */
            libusb_attach_kernel_driver(handle, i);
        }
    }
}

static int send_mode_switch(libusb_device_handle *handle)
{
    for (size_t i = 0; i < DIGIMEND_REPORT_COUNT; ++i) {
        int transferred = libusb_control_transfer(
            handle, MTM_REQUEST_TYPE, MTM_SET_REPORT, MTM_REPORT_TYPE_AND_ID,
            2, (unsigned char *)digimend_reports[i], MTM_REPORT_LENGTH,
            MTM_CONTROL_TRANSFER_MS);
        if (transferred != MTM_REPORT_LENGTH) {
            fprintf(stderr, "Mode switch report %zu failed (%s); retrying on next cycle.\n",
                    i + 1,
                    transferred < 0 ? libusb_error_name(transferred) : "short write");
            return -1;
        }
    }
    puts("Full-area mode switch sent.");
    return 0;
}

/*
 * Locate the pen data endpoint: the first interrupt IN endpoint whose
 * max packet size is 64 bytes (full-area report), on the highest-numbered
 * HID interface as a tie-breaker. Falls back to any interrupt IN endpoint.
 */
static int find_data_endpoint(libusb_device *device, uint8_t *address_out,
                              int *num_interfaces_out)
{
    struct libusb_config_descriptor *config = NULL;
    int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != 0 || config == NULL)
        return -1;

    uint8_t best_ep = 0;
    uint16_t best_size = 0;
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *interface = &config->interface[i];
        for (int j = 0; j < interface->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *descriptor =
                &interface->altsetting[j];
            if (descriptor->bInterfaceClass != LIBUSB_CLASS_HID)
                continue;
            for (uint8_t k = 0; k < descriptor->bNumEndpoints; ++k) {
                const struct libusb_endpoint_descriptor *endpoint =
                    &descriptor->endpoint[k];
                if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                    LIBUSB_TRANSFER_TYPE_INTERRUPT &&
                    (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) &&
                    endpoint->wMaxPacketSize >= best_size) {
                    best_ep = endpoint->bEndpointAddress;
                    best_size = endpoint->wMaxPacketSize;
                }
            }
        }
    }
    libusb_free_config_descriptor(config);
    if (best_ep == 0)
        return -1;
    *address_out = best_ep;
    (void)num_interfaces_out;
    return 0;
}

static int emit_uinput_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event = {
        .type = type,
        .code = code,
        .value = value,
    };
    if (write(fd, &event, sizeof(event)) != sizeof(event))
        return -1;
    return 0;
}

static int create_uinput_device(int *fd_out)
{
    int fd = open("/dev/uinput", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Could not open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    /*
     * Kernel-side uinput state machine (uinput.c): UI_DEV_CREATE only
     * succeeds when the device reached UIST_SETUP_COMPLETE, which the
     * write of the uinput_user_dev below triggers. Modern kernels also
     * receive axis resolution through UI_ABS_SETUP below.
     */
    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "MTM-1106 Pen");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = MTM_VENDOR_ID;
    uidev.id.product = MTM_PRODUCT_ID;
    uidev.absmax[ABS_X] = 4095;
    uidev.absmax[ABS_Y] = 4095;
    uidev.absmax[ABS_PRESSURE] = 2047;
    uidev.absmax[ABS_DISTANCE] = 10;
    uidev.absmin[ABS_DISTANCE] = 0;

    if (write(fd, &uidev, sizeof(uidev)) != sizeof(uidev)) {
        fprintf(stderr, "uinput write failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_PRESSURE);
    ioctl(fd, UI_SET_ABSBIT, ABS_DISTANCE);
#ifdef UI_ABS_SETUP
    struct uinput_abs_setup x_setup = {
        .code = ABS_X,
        .absinfo = { .minimum = 0, .maximum = 4095, .resolution = 100 },
    };
    struct uinput_abs_setup y_setup = {
        .code = ABS_Y,
        .absinfo = { .minimum = 0, .maximum = 4095, .resolution = 100 },
    };
    struct uinput_abs_setup pressure_setup = {
        .code = ABS_PRESSURE,
        .absinfo = { .minimum = 0, .maximum = 2047, .resolution = 1 },
    };
    if (ioctl(fd, UI_ABS_SETUP, &x_setup) != 0 ||
        ioctl(fd, UI_ABS_SETUP, &y_setup) != 0 ||
        ioctl(fd, UI_ABS_SETUP, &pressure_setup) != 0) {
        fprintf(stderr, "uinput axis resolution setup failed: %s\n", strerror(errno));
    }
#endif
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS2);
    for (size_t i = 0; i < sizeof(hotkey_keycodes) / sizeof(hotkey_keycodes[0]); ++i) {
        if (hotkey_keycodes[i] != 0)
            ioctl(fd, UI_SET_KEYBIT, hotkey_keycodes[i]);
    }

    if (ioctl(fd, UI_DEV_CREATE) != 0) {
        fprintf(stderr, "Could not create uinput device: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    puts("uinput tablet device created.");
    *fd_out = fd;
    return 0;
}

static void dispatch_report(int uinput_fd, const uint8_t *data, int length,
                            uint16_t *last_hotkey_bits, bool *pen_in_proximity)
{
    /*
     * 64-byte report layout (reverse-engineered from vin1060plus/mx002):
     *   data[1..2]  X axis little-endian (raw coordinate)
     *   data[3..4]  Y axis little-endian (raw coordinate)
     *   data[5..6]  pressure (2047 - raw)
     *   data[9]     pen buttons (4 = stylus, 6 = stylus2)
     *   data[11..12] tablet hotkey bitmask (active-low pairs)
     */
    // Track pen proximity state: cursor only appears after BTN_TOOL_PEN=1

    if (length < 13)
        return;

    // The firmware's raw origin matches the physical top-left corner.
    // Do not reverse both axes: that rotates the tablet by 180 degrees.
    int x = (int)data[2] | ((int)data[1] << 8);
    int y = (int)data[4] | ((int)data[3] << 8);
    int pressure = 2047 - ((int)data[6] | ((int)data[5] << 8));
    // data[7] carries distance: 0 = in range (hovering/touching), >0 = out of range
    bool pen_out_of_range = ((int)data[7]) > 0;

    // Emit distance first (libinput uses ABS_DISTANCE for hover detection)
    int distance = pen_out_of_range ? 0 : 10;
    emit_uinput_event(uinput_fd, EV_ABS, ABS_DISTANCE, distance);
    emit_uinput_event(uinput_fd, EV_ABS, ABS_X, x);
    emit_uinput_event(uinput_fd, EV_ABS, ABS_Y, y);
    emit_uinput_event(uinput_fd, EV_ABS, ABS_PRESSURE, pressure);

    bool touching = pressure > 0;
    // Cursor display requires a proper proximity lifecycle: BTN_TOOL_PEN=1 on
    // enter, BTN_TOOL_PEN=0 on leave. Emitting it as a constant 1 confuses
    // libinput/wayland compositors into never showing the cursor.
    if (!*pen_in_proximity && !pen_out_of_range) {
        emit_uinput_event(uinput_fd, EV_KEY, BTN_TOOL_PEN, 1);
        *pen_in_proximity = true;
    } else if (*pen_in_proximity && pen_out_of_range) {
        emit_uinput_event(uinput_fd, EV_KEY, BTN_TOUCH, 0);
        emit_uinput_event(uinput_fd, EV_KEY, BTN_TOOL_PEN, 0);
        *pen_in_proximity = false;
    }
    if (*pen_in_proximity) {
        emit_uinput_event(uinput_fd, EV_KEY, BTN_TOUCH, touching ? 1 : 0);
    }

    uint8_t pen = data[9];
    emit_uinput_event(uinput_fd, EV_KEY, BTN_STYLUS, pen == 4 ? 1 : 0);
    emit_uinput_event(uinput_fd, EV_KEY, BTN_STYLUS2, pen == 6 ? 1 : 0);

    /* Hotkeys: two's-complement masks; 255/51 means no key. */
    uint16_t hotkey_bits = (uint16_t)data[11] | ((uint16_t)data[12] << 8);
    uint16_t changed = hotkey_bits ^ *last_hotkey_bits;
    for (size_t i = 0; i < sizeof(hotkey_keycodes) / sizeof(hotkey_keycodes[0]); ++i) {
        if ((changed & (1u << i)) != 0 && hotkey_keycodes[i] != 0) {
            /* Bit set = released (active-low encoding). */
            int value = (hotkey_bits & (1u << i)) ? 0 : 1;
            emit_uinput_event(uinput_fd, EV_KEY, hotkey_keycodes[i], value);
        }
    }
    *last_hotkey_bits = hotkey_bits;

    emit_uinput_event(uinput_fd, EV_SYN, SYN_REPORT, 0);
}

static int run_session(libusb_context *context, libusb_device *device,
                       int uinput_fd)
{
    (void)context;
    libusb_device_handle *handle = NULL;
    int rc = libusb_open(device, &handle);
    if (rc != 0 || handle == NULL) {
        fprintf(stderr, "Could not open tablet: %s\n", libusb_error_name(rc));
        return -1;
    }
    libusb_set_auto_detach_kernel_driver(handle, 1);

    struct libusb_config_descriptor *config = NULL;
    int num_interfaces = 3;
    if (libusb_get_active_config_descriptor(device, &config) == 0 &&
        config != NULL) {
        num_interfaces = (int)config->bNumInterfaces;
        libusb_free_config_descriptor(config);
    }
    if (num_interfaces < 1)
        num_interfaces = 1;

    /*
     * Avoid resetting the device here: a reset re-enumerates the tablet
     * while we still hold the handle, which made every following
     * interrupt transfer fail with PIPE and triggered the false
     * "disconnected/reappeared" storm seen in the journal. The claim
     * below detaches the kernel drivers we need, which is sufficient.
     */
    bool claimed[8] = {false};
    int claim_count = claim_all(handle, num_interfaces, claimed);
    if (claim_count == 0) {
        fprintf(stderr, "Could not claim any interface; the kernel owns them.\n");
        release_all(handle, claimed, num_interfaces);
        libusb_close(handle);
        return -1;
    }
    printf("Claimed %d of %d interfaces (kernel keeps the rest).\n",
           claim_count, num_interfaces);

    if (send_mode_switch(handle) != 0) {
        release_all(handle, claimed, num_interfaces);
        libusb_close(handle);
        return -1;
    }

    uint8_t data_ep = 0;
    if (find_data_endpoint(device, &data_ep, &num_interfaces) != 0) {
        fprintf(stderr, "No interrupt IN data endpoint found; staying in control mode.\n");
        release_all(handle, claimed, num_interfaces);
        libusb_close(handle);
        return -1;
    }
    printf("Reading pen events from endpoint 0x%02x.\n", data_ep);

    int result = 0;
    uint16_t last_hotkey_bits = 0;
    bool pen_in_proximity = false;
    while (running) {
        uint8_t buffer[64];
        int length = 0;
        rc = libusb_interrupt_transfer(handle, data_ep, buffer, sizeof(buffer),
                                       &length, MTM_EVENT_TRANSFER_MS);
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            puts("Tablet disconnected; waiting for reconnection...");
            result = -1;
            break;
        }
        if (rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE) {
            /*
             * IO/PIPE is not necessarily a disconnect: it also happens
             * when the kernel re-attached one of the interfaces we
             * claimed or after a mid-session reset. Log it so the false
             * "disconnected/reappeared" storm (seen with the old reset
             * logic) can be diagnosed, then retry instead of tearing
             * down the session.
             */
            fprintf(stderr, "Endpoint 0x%02x transfer error: %s\n",
                    data_ep, libusb_error_name(rc));
            continue;
        }
        if (rc == LIBUSB_ERROR_TIMEOUT || rc == LIBUSB_ERROR_OVERFLOW)
            continue;
        if (rc == 0 && length > 0) {
            printf("Report received: %d bytes.\n", length);
            dispatch_report(uinput_fd, buffer, length, &last_hotkey_bits, &pen_in_proximity);
        }
    }

    release_all(handle, claimed, num_interfaces);
    libusb_close(handle);
    return result;
}

/* NOTE: this daemon deliberately has no runtime udev dependency;
 * libusb's device enumeration is sufficient for tracking reconnections. */
static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [options]\n\n"
            "Userspace driver for the MTM-1106/T501 (08f2:6811): switches the\n"
            "tablet to full-area (PC) mode and injects pen/hotkey events via uinput.\n"
            "The kernel hid-generic driver is bypassed, so the 8-byte mobile-area\n"
            "descriptor it caches does not restrict the active area.\n\n"
            "Options:\n"
            "  --help    Show this help\n",
            program);
}

int main(int argc, char **argv)
{
    struct sigaction action = { .sa_handler = signal_handler };
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int option;
    while ((option = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
        switch (option) {
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    libusb_context *context = NULL;
    if (libusb_init(&context) != 0) {
        fprintf(stderr, "Could not initialize libusb.\n");
        return EXIT_FAILURE;
    }

    int uinput_fd = 0;
    if (create_uinput_device(&uinput_fd) != 0) {
        libusb_exit(context);
        return EXIT_FAILURE;
    }

    int status = EXIT_FAILURE;
    while (running) {
        libusb_device *device = NULL;
        if (wait_for_tablet(context, &device) != 0)
            break;
        int session = run_session(context, device, uinput_fd);
        libusb_unref_device(device);
        if (session == 0) {
            status = EXIT_SUCCESS;
            break;
        }
        usleep(MTM_RECONNECT_SLEEP_MS * 1000);
    }

    if (uinput_fd > 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
    }
    libusb_exit(context);
    puts("mtm1106-daemon exited.");
    return status;
}
