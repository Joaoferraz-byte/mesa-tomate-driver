#include <errno.h>
#include <getopt.h>
#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MTM_VENDOR_ID 0x08f2u
#define MTM_PRODUCT_ID 0x6811u
#define MTM_INTERFACE 2
#define MTM_REPORT_TYPE_AND_ID 0x0308u
#define MTM_REQUEST_TYPE 0x21u
#define MTM_SET_REPORT 0x09u
#define MTM_REPORT_LENGTH 8u
#define MTM_TIMEOUT_MS 250u
#define MTM_MAX_ATTEMPTS 3
#define MTM_RETRY_SLEEP_MS 500u

struct report {
    const uint8_t bytes[MTM_REPORT_LENGTH];
};

static const struct report digimend_reports[] = {
    {{0x08, 0x04, 0x1d, 0x01, 0xff, 0xff, 0x06, 0x2e}},
    {{0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0}},
    {{0x08, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {{0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0}},
};

static const struct report mx002_reports[] = {
    {{0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0}},
};

enum profile {
    PROFILE_DIGIMEND,
    PROFILE_MX002,
};

struct options {
    enum profile profile;
    bool dry_run;
    bool self_test;
    int bus;
    int address;
};

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [options]\n\n"
            "Activates full-area (PC) mode for MTM-1106/T501 (08f2:6811).\n\n"
            "Options:\n"
            "  --profile digimend|mx002  Report profile (default: digimend)\n"
            "  --bus N                   Restrict to USB bus N\n"
            "  --address N               Restrict to USB address N\n"
            "  --dry-run                 Show what would be sent without opening USB\n"
            "  --self-test               Validate embedded vectors and exit\n"
            "  --help                    Show this help\n",
            program);
}

static int parse_profile(const char *value, enum profile *profile)
{
    if (strcmp(value, "digimend") == 0) {
        *profile = PROFILE_DIGIMEND;
        return 0;
    }
    if (strcmp(value, "mx002") == 0) {
        *profile = PROFILE_MX002;
        return 0;
    }
    fprintf(stderr, "Invalid profile: %s (use digimend or mx002).\n", value);
    return -1;
}

static int parse_nonnegative(const char *value, int *result, const char *name)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 0 || parsed > 255) {
        fprintf(stderr, "Invalid %s: %s\n", name, value);
        return -1;
    }
    *result = (int)parsed;
    return 0;
}

static const struct report *selected_reports(enum profile profile, size_t *count)
{
    if (profile == PROFILE_MX002) {
        *count = sizeof(mx002_reports) / sizeof(mx002_reports[0]);
        return mx002_reports;
    }
    *count = sizeof(digimend_reports) / sizeof(digimend_reports[0]);
    return digimend_reports;
}

static void print_reports(enum profile profile)
{
    size_t count = 0;
    const struct report *reports = selected_reports(profile, &count);

    printf("VID:PID %04x:%04x, HID interface %d, wValue 0x%04x\n",
           MTM_VENDOR_ID, MTM_PRODUCT_ID, MTM_INTERFACE, MTM_REPORT_TYPE_AND_ID);
    for (size_t i = 0; i < count; ++i) {
        printf("report[%zu]:", i + 1);
        for (size_t j = 0; j < MTM_REPORT_LENGTH; ++j)
            printf(" %02x", reports[i].bytes[j]);
        putchar('\n');
    }
}

static int self_test(void)
{
    size_t count = 0;
    const struct report *reports = selected_reports(PROFILE_DIGIMEND, &count);
    static const uint8_t expected_first[MTM_REPORT_LENGTH] =
        {0x08, 0x04, 0x1d, 0x01, 0xff, 0xff, 0x06, 0x2e};
    static const uint8_t expected_last[MTM_REPORT_LENGTH] =
        {0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0};

    if (count != 4 || memcmp(reports[0].bytes, expected_first, MTM_REPORT_LENGTH) != 0 ||
        memcmp(reports[3].bytes, expected_last, MTM_REPORT_LENGTH) != 0) {
        fprintf(stderr, "Profile vector failure: digimend.\n");
        return EXIT_FAILURE;
    }
    reports = selected_reports(PROFILE_MX002, &count);
    if (count != 1 || memcmp(reports[0].bytes, expected_last, MTM_REPORT_LENGTH) != 0) {
        fprintf(stderr, "Profile vector failure: mx002.\n");
        return EXIT_FAILURE;
    }
    puts("self-test: OK; no USB devices were opened.");
    return EXIT_SUCCESS;
}

static bool has_hid_interface(libusb_device *device, int wanted_interface)
{
    struct libusb_config_descriptor *config = NULL;
    int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != 0)
        rc = libusb_get_config_descriptor(device, 0, &config);
    if (rc != 0 || config == NULL)
        return false;

    bool found = false;
    for (uint8_t i = 0; i < config->bNumInterfaces && !found; ++i) {
        const struct libusb_interface *interface = &config->interface[i];
        for (int j = 0; j < interface->num_altsetting; ++j) {
            const struct libusb_interface_descriptor *descriptor = &interface->altsetting[j];
            if (descriptor->bInterfaceNumber == wanted_interface &&
                descriptor->bInterfaceClass == LIBUSB_CLASS_HID) {
                found = true;
                break;
            }
        }
    }
    libusb_free_config_descriptor(config);
    return found;
}

static int open_unique_target(libusb_context *context, const struct options *options,
                              libusb_device **device_out, libusb_device_handle **handle_out)
{
    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) {
        fprintf(stderr, "Could not enumerate USB: %s\n", libusb_error_name((int)count));
        return -1;
    }

    libusb_device *selected = NULL;
    size_t matches = 0;
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor descriptor;
        libusb_device *candidate = list[i];
        if (libusb_get_device_descriptor(candidate, &descriptor) != 0 ||
            descriptor.idVendor != MTM_VENDOR_ID || descriptor.idProduct != MTM_PRODUCT_ID)
            continue;
        if (options->bus >= 0 && (int)libusb_get_bus_number(candidate) != options->bus)
            continue;
        if (options->address >= 0 && (int)libusb_get_device_address(candidate) != options->address)
            continue;
        if (!has_hid_interface(candidate, MTM_INTERFACE))
            continue;
        selected = candidate;
        ++matches;
    }

    if (matches == 0) {
        fprintf(stderr,
                "No MTM-1106/T501 with HID interface %d found (%04x:%04x).\n",
                MTM_INTERFACE, MTM_VENDOR_ID, MTM_PRODUCT_ID);
        libusb_free_device_list(list, 1);
        return -1;
    }
    if (matches > 1) {
        fprintf(stderr,
                "Found %zu compatible tablets; use --bus and --address to select one.\n",
                matches);
        libusb_free_device_list(list, 1);
        return -1;
    }

    libusb_ref_device(selected);
    libusb_device_handle *handle = NULL;
    int rc = libusb_open(selected, &handle);
    libusb_free_device_list(list, 1);
    if (rc != 0 || handle == NULL) {
        libusb_unref_device(selected);
        fprintf(stderr, "Could not open tablet: %s\n", libusb_error_name(rc));
        return -1;
    }
    *device_out = selected;
    *handle_out = handle;
    return 0;
}

/*
 * Detach hid-generic from every interface so libusb can claim the target.
 * Failure to detach is non-fatal (interface may be free already).
 */
static void detach_all_kernel_drivers(libusb_device_handle *handle)
{
    for (int i = 0; i <= MTM_INTERFACE; ++i) {
        int rc = libusb_detach_kernel_driver(handle, i);
        if (rc != 0 && rc != LIBUSB_ERROR_NOT_FOUND &&
            rc != LIBUSB_ERROR_NOT_SUPPORTED) {
            fprintf(stderr, "Warning: could not detach kernel driver from interface %d: %s\n",
                    i, libusb_error_name(rc));
        }
    }
}

/*
 * USB-level reset forces the tablet back to a clean enumeration state,
 * matching the reset performed by the Windows service startup path and the
 * mx002 reference driver. A reset failure is non-fatal.
 */
static void reset_device(libusb_device_handle *handle)
{
    int rc = libusb_reset_device(handle);
    if (rc != 0 && rc != LIBUSB_ERROR_NOT_FOUND) {
        fprintf(stderr, "Warning: device reset failed (%s); continuing anyway.\n",
                libusb_error_name(rc));
    }
    usleep(300 * 1000);
}

/*
 * Re-select the first configuration: after a reset the tablet re-enumerates
 * and configuration 1 (full work mode) must be active, as on Windows where
 * the vendor service claims the device right after enumeration.
 */
static int set_active_configuration(libusb_device_handle *handle)
{
    int rc = libusb_set_configuration(handle, 1);
    if (rc != 0 && rc != LIBUSB_ERROR_BUSY) {
        fprintf(stderr, "Warning: could not set configuration 1: %s\n",
                libusb_error_name(rc));
        return -1;
    }
    return 0;
}

static int claim_all_hid_interfaces(libusb_device_handle *handle,
                                      bool *claimed_out, int max_interface)
{
    int claimed_count = 0;
    for (int i = 0; i <= max_interface; ++i) {
        int rc = libusb_claim_interface(handle, i);
        if (rc == 0) {
            claimed_out[claimed_count++] = (bool)i;
        } else if (rc != LIBUSB_ERROR_BUSY && rc != LIBUSB_ERROR_NOT_FOUND) {
            fprintf(stderr, "Warning: could not claim interface %d: %s\n",
                    i, libusb_error_name(rc));
        }
    }
    return claimed_count;
}

static void release_claimed_interfaces(libusb_device_handle *handle,
                                       const bool *claimed, int count)
{
    for (int i = 0; i < count; ++i) {
        if (libusb_release_interface(handle, (int)claimed[i]) != 0)
            fprintf(stderr, "Warning: could not release interface %d\n", (int)claimed[i]);
    }
}

static int send_profile_once(libusb_device_handle *handle, enum profile profile)
{
    size_t count = 0;
    const struct report *reports = selected_reports(profile, &count);

    bool claimed[8] = {false};
    int claimed_count = claim_all_hid_interfaces(handle, claimed, MTM_INTERFACE);
    if (claimed_count == 0) {
        fprintf(stderr, "Could not claim HID interface %d: device busy or missing\n",
                MTM_INTERFACE);
        return -1;
    }

    int result = 0;
    for (size_t i = 0; i < count; ++i) {
        int transferred = libusb_control_transfer(
            handle, MTM_REQUEST_TYPE, MTM_SET_REPORT, MTM_REPORT_TYPE_AND_ID,
            MTM_INTERFACE, (unsigned char *)reports[i].bytes, MTM_REPORT_LENGTH,
            MTM_TIMEOUT_MS);
        if (transferred < 0) {
            fprintf(stderr, "SET_REPORT %zu failed: %s\n", i + 1, libusb_error_name(transferred));
            result = -1;
            break;
        }
        if (transferred != MTM_REPORT_LENGTH) {
            fprintf(stderr, "SET_REPORT %zu transferred %d/%u bytes.\n",
                    i + 1, transferred, MTM_REPORT_LENGTH);
            result = -1;
            break;
        }
        printf("SET_REPORT %zu/%zu sent.\n", i + 1, count);
    }

    release_claimed_interfaces(handle, claimed, claimed_count);
    if (result != 0)
        return -1;

    /*
     * Hand hid-generic back the interfaces: the kernel must re-probe
     * interface 2 to pick up the new work-mode report descriptor, which is
     * what exposes pen proximity (hover) to libinput without a replug.
     */
    for (int i = 0; i <= MTM_INTERFACE; ++i) {
        int rc = libusb_attach_kernel_driver(handle, i);
        if (rc != 0 && rc != LIBUSB_ERROR_NOT_FOUND &&
            rc != LIBUSB_ERROR_BUSY && rc != LIBUSB_ERROR_NOT_SUPPORTED)
            fprintf(stderr, "Warning: could not re-attach interface %d: %s\n",
                    i, libusb_error_name(rc));
    }
    return 0;
}

/*
 * Full activation attempt: detach kernel drivers, reset, reconfigure, claim
 * and send the profile sequence. Returns 0 on success, -1 otherwise.
 */
static int attempt_activation(libusb_device_handle *handle, enum profile profile)
{
    detach_all_kernel_drivers(handle);
    reset_device(handle);
    if (set_active_configuration(handle) != 0)
        return -1;
    return send_profile_once(handle, profile);
}

static int send_profile(libusb_device_handle *handle, enum profile profile)
{
    int rc = libusb_set_auto_detach_kernel_driver(handle, 1);
    if (rc != 0 && rc != LIBUSB_ERROR_NOT_SUPPORTED) {
        fprintf(stderr, "Failed to prepare auto-detach: %s\n", libusb_error_name(rc));
        return -1;
    }

    for (int attempt = 1; attempt <= MTM_MAX_ATTEMPTS; ++attempt) {
        int result = attempt_activation(handle, profile);
        if (result == 0) {
            if (attempt > 1)
                printf("Activation succeeded on attempt %d.\n", attempt);
            return 0;
        }
        if (attempt < MTM_MAX_ATTEMPTS) {
            fprintf(stderr, "Attempt %d failed; retrying in %u ms...\n",
                    attempt, MTM_RETRY_SLEEP_MS);
            usleep(MTM_RETRY_SLEEP_MS * 1000);
        }
    }
    fprintf(stderr, "All %d activation attempts failed.\n", MTM_MAX_ATTEMPTS);
    return -1;
}

int main(int argc, char **argv)
{
    struct options options = {
        .profile = PROFILE_DIGIMEND,
        .dry_run = false,
        .self_test = false,
        .bus = -1,
        .address = -1,
    };
    static const struct option long_options[] = {
        {"profile", required_argument, NULL, 'p'},
        {"bus", required_argument, NULL, 'b'},
        {"address", required_argument, NULL, 'a'},
        {"dry-run", no_argument, NULL, 'n'},
        {"self-test", no_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int option;
    while ((option = getopt_long(argc, argv, "p:b:a:nth", long_options, NULL)) != -1) {
        switch (option) {
        case 'p':
            if (parse_profile(optarg, &options.profile) != 0)
                return EXIT_FAILURE;
            break;
        case 'b':
            if (parse_nonnegative(optarg, &options.bus, "Bus") != 0)
                return EXIT_FAILURE;
            break;
        case 'a':
            if (parse_nonnegative(optarg, &options.address, "Address") != 0)
                return EXIT_FAILURE;
            break;
        case 'n':
            options.dry_run = true;
            break;
        case 't':
            options.self_test = true;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (options.self_test)
        return self_test();
    if (options.dry_run) {
        print_reports(options.profile);
        return EXIT_SUCCESS;
    }

    libusb_context *context = NULL;
    int rc = libusb_init(&context);
    if (rc != 0) {
        fprintf(stderr, "Could not initialize libusb: %s\n", libusb_error_name(rc));
        return EXIT_FAILURE;
    }

    libusb_device *device = NULL;
    libusb_device_handle *handle = NULL;
    int result = open_unique_target(context, &options, &device, &handle);
    if (result == 0) {
        printf("Tablet found on bus %u, address %u.\n",
               libusb_get_bus_number(device), libusb_get_device_address(device));
        result = send_profile(handle, options.profile);
        libusb_close(handle);
        libusb_unref_device(device);
    }
    libusb_exit(context);

    if (result == 0)
        puts("Requested mode sent. Validate area with libinput and reconnect tablet if necessary.");
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
