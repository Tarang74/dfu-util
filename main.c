/*
 * dfu-util
 *
 * Copyright 2007-2008 by OpenMoko, Inc.
 * Copyright 2010-2012 Stefan Schmidt
 * Copyright 2013-2014 Hans Petter Selasky <hps@bitfrost.no>
 * Copyright 2010-2021 Tormod Volden
 *
 * Originally written by Harald Welte <laforge@openmoko.org>
 *
 * Based on existing code of dfu-programmer-0.4
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libusb.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dfu.h"
#include "dfu_file.h"
#include "dfu_load.h"
#include "dfu_util.h"
#include "dfuse.h"
#include "portable.h"

int verbose = 0;

struct dfu_if *dfu_root = NULL;

char *match_path = NULL;
int match_vendor = -1;
int match_product = -1;
int match_vendor_dfu = -1;
int match_product_dfu = -1;
int match_config_index = -1;
int match_iface_index = -1;
int match_iface_alt_index = -1;
int match_devnum = -1;
const char *match_iface_alt_name = NULL;
const char *match_serial = NULL;
const char *match_serial_dfu = NULL;

static int parse_match_value(const char *str, int default_value) {
  char *remainder;
  int value;

  if (str == NULL) {
    value = default_value;
  } else if (*str == '*') {
    value = -1; /* Match anything */
  } else if (*str == '-') {
    value = 0x10000; /* Impossible vendor/product ID */
  } else {
    value = strtoul(str, &remainder, 16);
    if (remainder == str) {
      value = default_value;
    }
  }
  return value;
}

static void parse_vendprod(const char *str) {
  const char *comma;
  const char *colon;

  /* Default to match any DFU device in runtime or DFU mode */
  match_vendor = -1;
  match_product = -1;
  match_vendor_dfu = -1;
  match_product_dfu = -1;

  comma = strchr(str, ',');
  if (comma == str) {
    /* DFU mode vendor/product being specified without any runtime
     * vendor/product specification, so don't match any runtime device */
    match_vendor = match_product = 0x10000;
  } else {
    colon = strchr(str, ':');
    if (colon != NULL) {
      ++colon;
      if ((comma != NULL) && (colon > comma)) {
        colon = NULL;
      }
    }
    match_vendor = parse_match_value(str, match_vendor);
    match_product = parse_match_value(colon, match_product);
    if (comma != NULL) {
      /* Both runtime and DFU mode vendor/product specifications are
       * available, so default DFU mode match components to the given
       * runtime match components */
      match_vendor_dfu = match_vendor;
      match_product_dfu = match_product;
    }
  }
  if (comma != NULL) {
    ++comma;
    colon = strchr(comma, ':');
    if (colon != NULL) {
      ++colon;
    }
    match_vendor_dfu = parse_match_value(comma, match_vendor_dfu);
    match_product_dfu = parse_match_value(colon, match_product_dfu);
  }
}

static void parse_serial(char *str) {
  char *comma;

  match_serial = str;
  comma = strchr(str, ',');
  if (comma == NULL) {
    match_serial_dfu = match_serial;
  } else {
    *comma++ = 0;
    match_serial_dfu = comma;
  }
  if (*match_serial == 0)
    match_serial = NULL;
  if (*match_serial_dfu == 0)
    match_serial_dfu = NULL;
}

static int parse_number(char *str, char *nmb) {
  char *endptr;
  long val;

  errno = 0;
  val = strtol(nmb, &endptr, 0);

  if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN)) ||
      (errno != 0 && val == 0) || (*endptr != '\0')) {
    errx(EX_USAGE, "Something went wrong with the argument of --%s\n", str);
  }

  if (endptr == nmb) {
    errx(EX_USAGE, "No digits were found from the argument of --%s\n", str);
  }

  return (int)val;
}

static void help(void) {
  fprintf(stderr,
          "Usage: dfu-util [options] ...\n"
          "  -h --help\t\t\tPrint this help message\n"
          "  -V --version\t\t\tPrint the version number\n"
          "  -v --verbose\t\t\tPrint verbose debug statements\n"
          "  -l --list\t\t\tList currently attached DFU capable devices\n");
  fprintf(
      stderr,
      "  -e --detach\t\t\tDetach currently attached DFU capable devices\n"
      "  -E --detach-delay seconds\tTime to wait before reopening a device "
      "after detach\n"
      "  -d --device <vendor>:<product>[,<vendor_dfu>:<product_dfu>]\n"
      "\t\t\t\tSpecify Vendor/Product ID(s) of DFU device\n"
      "  -n --devnum <dnum>\t\tMatch given device number (devnum from --list)\n"
      "  -p --path <bus-port. ... .port>\tSpecify path to DFU device\n"
      "  -c --cfg <config_nr>\t\tSpecify the Configuration of DFU device\n"
      "  -i --intf <intf_nr>\t\tSpecify the DFU Interface number\n"
      "  -S --serial <serial_string>[,<serial_string_dfu>]\n"
      "\t\t\t\tSpecify Serial String of DFU device\n"
      "  -a --alt <alt>\t\tSpecify the Altsetting of the DFU Interface\n"
      "\t\t\t\tby name or by number\n");
  fprintf(
      stderr,
      "  -t --transfer-size <size>\tSpecify the number of bytes per USB "
      "Transfer\n"
      "  -U --upload <file>\t\tRead firmware from device into <file>\n"
      "  -Z --upload-size <bytes>\tSpecify the expected upload size in bytes\n"
      "  -D --download <file>\t\tWrite firmware from <file> into device\n"
      "  -R --reset\t\t\tIssue USB Reset signalling once we're finished\n"
      "  -w --wait\t\t\tWait for device to appear\n"
      "  -s --dfuse-address address<:...>\tST DfuSe mode string, specifying "
      "target\n"
      "\t\t\t\taddress for raw file download or upload (not\n"
      "\t\t\t\tapplicable for DfuSe file (.dfu) downloads).\n"
      "\t\t\t\tAdd more DfuSe options separated with ':'\n"
      "\t\tleave\t\tLeave DFU mode (jump to application)\n"
      "\t\tmass-erase\tErase the whole device (requires \"force\")\n"
      "\t\tunprotect\tErase read protected device (requires \"force\")\n"
      "\t\twill-reset\tExpect device to reset (e.g. option bytes write)\n"
      "\t\tforce\t\tYou really know what you are doing!\n"
      "\t\t<length>\tLength of firmware to upload from device\n");
}

static void print_version(void) {
  printf(PACKAGE_STRING "\n\n");
  printf("Copyright 2005-2009 Weston Schmidt, Harald Welte and OpenMoko Inc.\n"
         "Copyright 2010-2021 Tormod Volden and Stefan Schmidt\n"
         "This program is Free Software and has ABSOLUTELY NO WARRANTY\n"
         "Please report bugs to " PACKAGE_BUGREPORT "\n\n");
}

static const struct option opts[] = {
    {"help", 0, 0, 'h'},          {"version", 0, 0, 'V'},
    {"verbose", 0, 0, 'v'},       {"list", 0, 0, 'l'},
    {"detach", 0, 0, 'e'},        {"detach-delay", 1, 0, 'E'},
    {"device", 1, 0, 'd'},        {"path", 1, 0, 'p'},
    {"configuration", 1, 0, 'c'}, {"cfg", 1, 0, 'c'},
    {"interface", 1, 0, 'i'},     {"intf", 1, 0, 'i'},
    {"altsetting", 1, 0, 'a'},    {"alt", 1, 0, 'a'},
    {"serial", 1, 0, 'S'},        {"transfer-size", 1, 0, 't'},
    {"upload", 1, 0, 'U'},        {"upload-size", 1, 0, 'Z'},
    {"download", 1, 0, 'D'},      {"reset", 0, 0, 'R'},
    {"dfuse-address", 1, 0, 's'}, {"devnum", 1, 0, 'n'},
    {"wait", 1, 0, 'w'},          {0, 0, 0, 0}};

int main(int argc, char **argv) {
  int expected_size = 0;
  unsigned int transfer_size = 0;
  enum mode mode = MODE_NONE;
  struct dfu_status status;
  libusb_context *ctx;
  struct dfu_file file;
  char *end;
  int final_reset = 0;
  int wait_device = 0;
  int ret;
  int dfuse_device = 0;
  int fd;
  const char *dfuse_options = NULL;
  int detach_delay = 5;
  uint16_t runtime_vendor;
  uint16_t runtime_product;

  memset(&file, 0, sizeof(file));

  /* make sure all prints are flushed */
  setvbuf(stdout, NULL, _IONBF, 0);

  while (1) {
    int c, option_index = 0;
    c = getopt_long(argc, argv, "hVvleE:d:p:c:i:a:S:t:U:D:Rs:Z:wn:", opts,
                    &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 'h':
      help();
      exit(EX_OK);
      break;
    case 'V':
      mode = MODE_VERSION;
      break;
    case 'v':
      verbose++;
      break;
    case 'l':
      mode = MODE_LIST;
      break;
    case 'e':
      mode = MODE_DETACH;
      break;
    case 'E':
      detach_delay = parse_number("detach-delay", optarg);
      break;
    case 'd':
      parse_vendprod(optarg);
      break;
    case 'p':
#if (defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000102) ||       \
    (defined(LIBUSBX_API_VERSION) && LIBUSBX_API_VERSION >= 0x01000102)
      match_path = optarg;
#else
      errx(EX_SOFTWARE, "This dfu-util was built without USB path support");
#endif
      break;
    case 'c':
      /* Configuration */
      match_config_index = parse_number("cfg", optarg);
      break;
    case 'i':
      /* Interface */
      match_iface_index = parse_number("intf", optarg);
      break;
    case 'a':
      /* Interface Alternate Setting */
      match_iface_alt_index = strtoul(optarg, &end, 0);
      if (*end) {
        match_iface_alt_name = optarg;
        match_iface_alt_index = -1;
      }
      break;
    case 'n':
      match_devnum = atoi(optarg);
      break;
    case 'S':
      parse_serial(optarg);
      break;
    case 't':
      transfer_size = parse_number("transfer-size", optarg);
      break;
    case 'U':
      mode = MODE_UPLOAD;
      file.name = optarg;
      break;
    case 'Z':
      expected_size = parse_number("upload-size", optarg);
      break;
    case 'D':
      mode = MODE_DOWNLOAD;
      file.name = optarg;
      break;
    case 'R':
      final_reset = 1;
      break;
    case 's':
      dfuse_options = optarg;
      break;
    case 'w':
      wait_device = 1;
      break;
    default:
      help();
      exit(EX_USAGE);
      break;
    }
  }
  if (optind != argc) {
    fprintf(stderr, "Error: Unexpected argument: %s\n\n", argv[optind]);
    help();
    exit(EX_USAGE);
  }

  print_version();
  if (mode == MODE_VERSION) {
    exit(EX_OK);
  }

#if defined(LIBUSB_API_VERSION) || defined(LIBUSBX_API_VERSION)
  if (verbose) {
    const struct libusb_version *ver;
    ver = libusb_get_version();
    printf("libusb version %i.%i.%i%s (%i)\n", ver->major, ver->minor,
           ver->micro, ver->rc, ver->nano);
  }
#else
  warnx("libusb version is ancient");
#endif

  if (mode == MODE_NONE && !dfuse_options) {
    fprintf(stderr, "You need to specify one of -D or -U\n");
    help();
    exit(EX_USAGE);
  }

  if (match_config_index == 0) {
    /* Handle "-c 0" (unconfigured device) as don't care */
    match_config_index = -1;
  }

  if (mode == MODE_DOWNLOAD) {
    dfu_load_file(&file, MAYBE_SUFFIX, MAYBE_PREFIX);
    /* If the user didn't specify product and/or vendor IDs to match,
     * use any IDs from the file suffix for device matching */
    if (match_vendor < 0 && file.idVendor != 0xffff) {
      match_vendor = file.idVendor;
      printf("Match vendor ID from file: %04x\n", match_vendor);
    }
    if (match_product < 0 && file.idProduct != 0xffff) {
      match_product = file.idProduct;
      printf("Match product ID from file: %04x\n", match_product);
    }
  } else if (mode == MODE_NONE && dfuse_options) {
    /* for DfuSe special commands, match any device */
    mode = MODE_DOWNLOAD;
    file.idVendor = 0xffff;
    file.idProduct = 0xffff;
  }

  if (wait_device) {
    printf("Waiting for device, exit with ctrl-C\n");
  }

  ret = libusb_init(&ctx);
  if (ret)
    errx(EX_IOERR, "unable to initialize libusb: %s", libusb_error_name(ret));

  if (verbose > 2) {
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_DEBUG);
#else
    libusb_set_debug(ctx, 255);
#endif
  }
probe:
  probe_devices(ctx);

  if (mode == MODE_LIST) {
    list_dfu_interfaces();
    disconnect_devices();
    libusb_exit(ctx);
    return EX_OK;
  }

  if (dfu_root == NULL) {
    if (wait_device) {
      milli_sleep(20);
      goto probe;
    } else {
      warnx("No DFU capable USB device available");
      libusb_exit(ctx);
      return EX_IOERR;
    }
  } else if (file.bcdDFU == 0x11a && dfuse_multiple_alt(dfu_root)) {
    printf("Multiple alternate interfaces for DfuSe file\n");
  } else if (dfu_root->next != NULL) {
    /* We cannot safely support more than one DFU capable device
     * with same vendor/product ID, since during DFU we need to do
     * a USB bus reset, after which the target device will get a
     * new address */
    errx(EX_IOERR, "More than one DFU capable USB device found! "
                   "Try `--list' and specify the serial number "
                   "or disconnect all but one device\n");
  }

  /* We have exactly one device. Its libusb_device is now in dfu_root->dev */

  printf("Opening DFU capable USB device...\n");
  ret = libusb_open(dfu_root->dev, &dfu_root->dev_handle);
  if (ret || !dfu_root->dev_handle)
    errx(EX_IOERR, "Cannot open device: %s", libusb_error_name(ret));

  printf("Device ID %04x:%04x\n", dfu_root->vendor, dfu_root->product);

  /* If first interface is DFU it is likely not proper run-time */
  if (dfu_root->interface > 0)
    printf("Run-Time device");
  else
    printf("Device");
  printf(" DFU version %04x\n",
         libusb_le16_to_cpu(dfu_root->func_dfu.bcdDFUVersion));

  if (verbose) {
    printf("DFU attributes: (0x%02x)", dfu_root->func_dfu.bmAttributes);
    if (dfu_root->func_dfu.bmAttributes & USB_DFU_CAN_DOWNLOAD)
      printf(" bitCanDnload");
    if (dfu_root->func_dfu.bmAttributes & USB_DFU_CAN_UPLOAD)
      printf(" bitCanUpload");
    if (dfu_root->func_dfu.bmAttributes & USB_DFU_MANIFEST_TOL)
      printf(" bitManifestationTolerant");
    if (dfu_root->func_dfu.bmAttributes & USB_DFU_WILL_DETACH)
      printf(" bitWillDetach");
    printf("\n");
    printf("Detach timeout %d ms\n",
           libusb_le16_to_cpu(dfu_root->func_dfu.wDetachTimeOut));
  }

  /* Transition from run-Time mode to DFU mode */
  if (!(dfu_root->flags & DFU_IFF_DFU)) {
    int err;
    /* In the 'first round' during runtime mode, there can only be one
     * DFU Interface descriptor according to the DFU Spec. */

    /* FIXME: check if the selected device really has only one */

    runtime_vendor = dfu_root->vendor;
    runtime_product = dfu_root->product;

    printf("Claiming USB DFU (Run-Time) Interface...\n");
    ret = libusb_claim_interface(dfu_root->dev_handle, dfu_root->interface);
    if (ret < 0) {
      errx(EX_IOERR, "Cannot claim interface %d: %s", dfu_root->interface,
           libusb_error_name(ret));
    }

    /* Needed for some devices where the DFU interface is not the first,
     * and should also be safe if there are multiple alt settings.
     * Otherwise skip the request since it might not be supported
     * by the device and the USB stack may or may not recover */
    if (dfu_root->interface > 0 || dfu_root->flags & DFU_IFF_ALT) {
      printf("Setting Alternate Interface zero...\n");
      ret = libusb_set_interface_alt_setting(dfu_root->dev_handle,
                                             dfu_root->interface, 0);
      if (ret < 0) {
        errx(EX_IOERR, "Cannot set alternate interface zero: %s",
             libusb_error_name(ret));
      }
    }

    printf("Determining device status...\n");
    err = dfu_get_status(dfu_root, &status);
    if (err == LIBUSB_ERROR_PIPE) {
      printf("Device does not implement get_status, assuming appIDLE\n");
      status.bStatus = DFU_STATUS_OK;
      status.bwPollTimeout = 0;
      status.bState = DFU_STATE_appIDLE;
      status.iString = 0;
    } else if (err < 0) {
      errx(EX_IOERR, "error get_status: %s", libusb_error_name(err));
    } else {
      printf("DFU state(%u) = %s, status(%u) = %s\n", status.bState,
             dfu_state_to_string(status.bState), status.bStatus,
             dfu_status_to_string(status.bStatus));
    }
    milli_sleep(status.bwPollTimeout);

    switch (status.bState) {
    case DFU_STATE_appIDLE:
    case DFU_STATE_appDETACH:
      printf("Device really in Run-Time Mode, send DFU "
             "detach request...\n");
      if (dfu_detach(dfu_root->dev_handle, dfu_root->interface, 1000) < 0) {
        warnx("error detaching");
      }
      if (dfu_root->func_dfu.bmAttributes & USB_DFU_WILL_DETACH) {
        printf("Device will detach and reattach...\n");
      } else {
        printf("Resetting USB...\n");
        ret = libusb_reset_device(dfu_root->dev_handle);
        if (ret < 0 && ret != LIBUSB_ERROR_NOT_FOUND)
          errx(EX_IOERR,
               "error resetting "
               "after detach: %s",
               libusb_error_name(ret));
      }
      break;
    case DFU_STATE_dfuERROR:
      printf("dfuERROR, clearing status\n");
      if (dfu_clear_status(dfu_root->dev_handle, dfu_root->interface) < 0) {
        errx(EX_IOERR, "error clear_status");
      }
      /* fall through */
    default:
      warnx("WARNING: Device already in DFU mode? (bState=%d %s)",
            status.bState, dfu_state_to_string(status.bState));
      libusb_release_interface(dfu_root->dev_handle, dfu_root->interface);
      goto dfustate;
    }
    libusb_release_interface(dfu_root->dev_handle, dfu_root->interface);
    libusb_close(dfu_root->dev_handle);
    dfu_root->dev_handle = NULL;

    /* keeping handles open might prevent re-enumeration */
    disconnect_devices();

    if (mode == MODE_DETACH) {
      libusb_exit(ctx);
      return EX_OK;
    }

    milli_sleep(detach_delay * 1000);

    /* Change match vendor and product to impossible values to force
     * only DFU mode matches in the following probe */
    match_vendor = match_product = 0x10000;

    probe_devices(ctx);

    if (dfu_root == NULL) {
      errx(EX_IOERR, "Lost device after RESET?");
    } else if (dfu_root->next != NULL) {
      errx(EX_IOERR, "More than one DFU capable USB device found! "
                     "Try `--list' and specify the serial number "
                     "or disconnect all but one device");
    }

    /* Check for DFU mode device */
    if (!(dfu_root->flags | DFU_IFF_DFU))
      errx(EX_PROTOCOL, "Device is not in DFU mode");

    printf("Opening DFU USB Device...\n");
    ret = libusb_open(dfu_root->dev, &dfu_root->dev_handle);
    if (ret || !dfu_root->dev_handle) {
      errx(EX_IOERR, "Cannot open device");
    }
  } else {
    /* we're already in DFU mode, so we can skip the detach/reset
     * procedure */
    /* If a match vendor/product was specified, use that as the runtime
     * vendor/product, otherwise use the DFU mode vendor/product */
    runtime_vendor = match_vendor < 0 ? dfu_root->vendor : match_vendor;
    runtime_product = match_product < 0 ? dfu_root->product : match_product;
  }

dfustate:
#if 0
	printf("Setting Configuration %u...\n", dfu_root->configuration);
	ret = libusb_set_configuration(dfu_root->dev_handle, dfu_root->configuration);
	if (ret < 0) {
		errx(EX_IOERR, "Cannot set configuration: %s", libusb_error_name(ret));
	}
#endif
  printf("Claiming USB DFU Interface...\n");
  ret = libusb_claim_interface(dfu_root->dev_handle, dfu_root->interface);
  if (ret < 0) {
    errx(EX_IOERR, "Cannot claim interface - %s", libusb_error_name(ret));
  }

  if (dfu_root->flags & DFU_IFF_ALT) {
    printf("Setting Alternate Interface #%d ...\n", dfu_root->altsetting);
    ret = libusb_set_interface_alt_setting(
        dfu_root->dev_handle, dfu_root->interface, dfu_root->altsetting);
    if (ret < 0) {
      errx(EX_IOERR, "Cannot set alternate interface: %s",
           libusb_error_name(ret));
    }
  }

status_again:
  printf("Determining device status...\n");
  ret = dfu_get_status(dfu_root, &status);
  if (ret < 0) {
    errx(EX_IOERR, "error get_status: %s", libusb_error_name(ret));
  }
  printf("DFU state(%u) = %s, status(%u) = %s\n", status.bState,
         dfu_state_to_string(status.bState), status.bStatus,
         dfu_status_to_string(status.bStatus));

  milli_sleep(status.bwPollTimeout);

  switch (status.bState) {
  case DFU_STATE_appIDLE:
  case DFU_STATE_appDETACH:
    errx(EX_PROTOCOL, "Device still in Run-Time Mode!");
    break;
  case DFU_STATE_dfuERROR:
    printf("Clearing status\n");
    if (dfu_clear_status(dfu_root->dev_handle, dfu_root->interface) < 0) {
      errx(EX_IOERR, "error clear_status");
    }
    goto status_again;
    break;
  case DFU_STATE_dfuDNLOAD_IDLE:
  case DFU_STATE_dfuUPLOAD_IDLE:
    printf("Aborting previous incomplete transfer\n");
    if (dfu_abort(dfu_root->dev_handle, dfu_root->interface) < 0) {
      errx(EX_IOERR, "can't send DFU_ABORT");
    }
    goto status_again;
    break;
  case DFU_STATE_dfuIDLE:
  default:
    break;
  }

  if (DFU_STATUS_OK != status.bStatus) {
    printf("WARNING: DFU Status: '%s'\n", dfu_status_to_string(status.bStatus));
    /* Clear our status & try again. */
    if (dfu_clear_status(dfu_root->dev_handle, dfu_root->interface) < 0)
      errx(EX_IOERR, "USB communication error");
    if (dfu_get_status(dfu_root, &status) < 0)
      errx(EX_IOERR, "USB communication error");
    if (DFU_STATUS_OK != status.bStatus)
      errx(EX_PROTOCOL, "Status is not OK: %d", status.bStatus);

    milli_sleep(status.bwPollTimeout);
  }

  printf("DFU mode device DFU version %04x\n",
         libusb_le16_to_cpu(dfu_root->func_dfu.bcdDFUVersion));

  if (dfu_root->func_dfu.bcdDFUVersion == libusb_cpu_to_le16(0x11a))
    dfuse_device = 1;
  else if (dfuse_options)
    printf("Warning: DfuSe option used on non-DfuSe device\n");

  /* Get from device or user, warn if overridden */
  int func_dfu_transfer_size =
      libusb_le16_to_cpu(dfu_root->func_dfu.wTransferSize);
  if (func_dfu_transfer_size) {
    printf("Device returned transfer size %i\n", func_dfu_transfer_size);
    if (!transfer_size)
      transfer_size = func_dfu_transfer_size;
    else
      printf("Warning: Overriding device-reported transfer size\n");
  } else {
    if (!transfer_size)
      errx(EX_USAGE, "Transfer size must be specified");
  }

#ifdef __linux__
  /* limited to 4k in libusb Linux backend */
  if ((int)transfer_size > 4096) {
    transfer_size = 4096;
    printf("Limited transfer size to %i\n", transfer_size);
  }
#endif /* __linux__ */

  if (transfer_size < dfu_root->bMaxPacketSize0) {
    transfer_size = dfu_root->bMaxPacketSize0;
    printf("Adjusted transfer size to %i\n", transfer_size);
  }

  switch (mode) {
  case MODE_UPLOAD:
    /* open for "exclusive" writing */
    fd =
        open(file.name, O_WRONLY | O_BINARY | O_CREAT | O_EXCL | O_TRUNC, 0666);
    if (fd < 0) {
      warn("Cannot open file %s for writing", file.name);
      ret = EX_CANTCREAT;
      break;
    }

    if (dfuse_device || dfuse_options) {
      ret = dfuse_do_upload(dfu_root, transfer_size, fd, dfuse_options);
    } else {
      ret = dfuload_do_upload(dfu_root, transfer_size, expected_size, fd);
    }
    close(fd);
    if (ret < 0)
      ret = EX_IOERR;
    else
      ret = EX_OK;
    break;

  case MODE_DOWNLOAD:
    if (((file.idVendor != 0xffff && file.idVendor != runtime_vendor) ||
         (file.idProduct != 0xffff && file.idProduct != runtime_product)) &&
        ((file.idVendor != 0xffff && file.idVendor != dfu_root->vendor) ||
         (file.idProduct != 0xffff && file.idProduct != dfu_root->product))) {
      errx(EX_USAGE,
           "Error: File ID %04x:%04x does "
           "not match device (%04x:%04x or %04x:%04x)",
           file.idVendor, file.idProduct, runtime_vendor, runtime_product,
           dfu_root->vendor, dfu_root->product);
    }
    if (dfuse_device || dfuse_options || file.bcdDFU == 0x11a) {
      ret = dfuse_do_dnload(dfu_root, transfer_size, &file, dfuse_options);
    } else {
      ret = dfuload_do_dnload(dfu_root, transfer_size, &file);
    }
    if (ret < 0)
      ret = EX_IOERR;
    else
      ret = EX_OK;
    break;
  case MODE_DETACH:
    ret = dfu_detach(dfu_root->dev_handle, dfu_root->interface, 1000);
    if (ret < 0) {
      warnx("can't detach");
      /* allow combination with final_reset */
      ret = 0;
    }
    break;
  default:
    warnx("Unsupported mode: %u", mode);
    ret = EX_SOFTWARE;
    break;
  }

  if (!ret && final_reset) {
    ret = dfu_detach(dfu_root->dev_handle, dfu_root->interface, 1000);
    if (ret < 0) {
      /* Even if detach failed, just carry on to leave the
         device in a known state */
      warnx("can't detach");
    }
    printf("Resetting USB to switch back to Run-Time mode\n");
    ret = libusb_reset_device(dfu_root->dev_handle);
    if (ret < 0 && ret != LIBUSB_ERROR_NOT_FOUND) {
      warnx("error resetting after download: %s", libusb_error_name(ret));
      ret = EX_IOERR;
    }
  }

  libusb_close(dfu_root->dev_handle);
  dfu_root->dev_handle = NULL;

  disconnect_devices();
  libusb_exit(ctx);
  return ret;
}
