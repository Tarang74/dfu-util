/*
 * Functions for detecting DFU USB entities
 *
 * Written by Harald Welte <laforge@openmoko.org>
 * Copyright 2007-2008 by OpenMoko, Inc.
 * Copyright 2013 Hans Petter Selasky <hps@bitfrost.no>
 * Copyright 2016-2019 Tormod Volden <debian.tormod@gmail.com>
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
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dfu.h"
#include "dfu_file.h"
#include "dfu_util.h"
#include "portable.h"
#include "quirks.h"
#include "usb_dfu.h"

/*
 * Look for a descriptor in a concatenated descriptor list. Will
 * return upon the first match of the given descriptor type. Returns length of
 * found descriptor, limited to res_size
 */
static int find_descriptor(const uint8_t *desc_list, int list_len,
                           uint8_t desc_type, void *res_buf, int res_size) {
  int p = 0;

  if (list_len < 2)
    return (-1);

  while (p + 1 < list_len) {
    int desclen;

    desclen = (int)desc_list[p];
    if (desclen == 0) {
      warnx("Invalid descriptor list");
      return -1;
    }
    if (desc_list[p + 1] == desc_type) {
      if (desclen > res_size)
        desclen = res_size;
      if (p + desclen > list_len)
        desclen = list_len - p;
      memcpy(res_buf, &desc_list[p], desclen);
      return desclen;
    }
    p += (int)desc_list[p];
  }
  return -1;
}

/*
 * Get a string descriptor that's UTF-8 (or ASCII) encoded instead
 * of UTF-16 encoded like the USB specification mandates. Some
 * devices, like the GD32VF103, both violate the spec in this way
 * and store important information in the serial number field. This
 * function does NOT append a NUL terminator to its buffer, so you
 * must use the returned length to ensure you stay within bounds.
 */
static int get_utf8_string_descriptor(libusb_device_handle *devh,
                                      uint8_t desc_index, unsigned char *data,
                                      int length) {
  unsigned char tbuf[255];
  uint16_t langid;
  int r, outlen;

  /* get the language IDs and pick the first one */
  r = libusb_get_string_descriptor(devh, 0, 0, tbuf, sizeof(tbuf));
  if (r < 0) {
    warnx("Failed to retrieve language identifiers");
    return r;
  }
  if (r < 4 || tbuf[0] < 4 ||
      tbuf[1] != LIBUSB_DT_STRING) { /* must have at least one ID */
    warnx("Broken LANGID string descriptor");
    return -1;
  }
  langid = tbuf[2] | (tbuf[3] << 8);

  r = libusb_get_string_descriptor(devh, desc_index, langid, tbuf,
                                   sizeof(tbuf));
  if (r < 0) {
    warnx("Failed to retrieve string descriptor %d", desc_index);
    return r;
  }
  if (r < 2 || tbuf[0] < 2) {
    warnx("String descriptor %d too short", desc_index);
    return -1;
  }
  if (tbuf[1] != LIBUSB_DT_STRING) { /* sanity check */
    warnx("Malformed string descriptor %d, type = 0x%02x", desc_index, tbuf[1]);
    return -1;
  }
  if (tbuf[0] > r) { /* if short read,           */
    warnx("Patching string descriptor %d length (was %d, received %d)",
          desc_index, tbuf[0], r);
    tbuf[0] = r; /* fix up descriptor length */
  }

  outlen = tbuf[0] - 2;
  if (length < outlen)
    outlen = length;

  memcpy(data, tbuf + 2, outlen);

  return outlen;
}

/*
 * Similar to libusb_get_string_descriptor_ascii but will allow
 * truncated descriptors (descriptor length mismatch) seen on
 * e.g. the STM32F427 ROM bootloader.
 */
static int get_string_descriptor_ascii(libusb_device_handle *devh,
                                       uint8_t desc_index, unsigned char *data,
                                       int length) {
  unsigned char buf[255];
  int r, di, si;

  r = get_utf8_string_descriptor(devh, desc_index, buf, sizeof(buf));
  if (r < 0)
    return r;

  /* convert from 16-bit unicode to ascii string */
  for (di = 0, si = 0; si + 1 < r && di < length; si += 2) {
    if (buf[si + 1]) /* high byte of unicode char */
      data[di++] = '?';
    else
      data[di++] = buf[si];
  }
  data[di] = '\0';
  return di;
}

static void probe_configuration(libusb_device *dev,
                                struct libusb_device_descriptor *desc) {
  struct usb_dfu_func_descriptor func_dfu;
  libusb_device_handle *devh;
  struct dfu_if *pdfu;
  struct libusb_config_descriptor *cfg;
  const struct libusb_interface_descriptor *intf;
  const struct libusb_interface *uif;
  char alt_name[MAX_DESC_STR_LEN + 1];
  char serial_name[MAX_DESC_STR_LEN + 1];
  int cfg_idx;
  int intf_idx;
  int alt_idx;
  int ret;
  int has_dfu;

  for (cfg_idx = 0; cfg_idx != desc->bNumConfigurations; cfg_idx++) {
    memset(&func_dfu, 0, sizeof(func_dfu));
    has_dfu = 0;

    ret = libusb_get_config_descriptor(dev, cfg_idx, &cfg);
    if (ret != 0)
      return;
    if (match_config_index > -1 &&
        match_config_index != cfg->bConfigurationValue) {
      libusb_free_config_descriptor(cfg);
      continue;
    }

    /*
     * In some cases, noticably FreeBSD if uid != 0,
     * the configuration descriptors are empty
     */
    if (!cfg)
      return;

    ret = find_descriptor(cfg->extra, cfg->extra_length, USB_DT_DFU, &func_dfu,
                          sizeof(func_dfu));
    if (ret > -1)
      goto found_dfu;

    for (intf_idx = 0; intf_idx < cfg->bNumInterfaces; intf_idx++) {
      uif = &cfg->interface[intf_idx];
      if (!uif)
        break;

      for (alt_idx = 0; alt_idx < cfg->interface[intf_idx].num_altsetting;
           alt_idx++) {
        intf = &uif->altsetting[alt_idx];

        if (intf->bInterfaceClass != 0xfe || intf->bInterfaceSubClass != 1)
          continue;

        ret = find_descriptor(intf->extra, intf->extra_length, USB_DT_DFU,
                              &func_dfu, sizeof(func_dfu));
        if (ret > -1)
          goto found_dfu;

        has_dfu = 1;
      }
    }
    if (has_dfu) {
      /*
       * Finally try to retrieve it requesting the
       * device directly This is not supported on
       * all devices for non-standard types
       */
      if (libusb_open(dev, &devh) == 0) {
        ret = libusb_get_descriptor(devh, USB_DT_DFU, 0, (void *)&func_dfu,
                                    sizeof(func_dfu));
        libusb_close(devh);
        if (ret > -1)
          goto found_dfu;
      }
      warnx("Device has DFU interface, "
            "but has no DFU functional descriptor");

      /* fake version 1.0 */
      func_dfu.bLength = 7;
      func_dfu.bcdDFUVersion = libusb_cpu_to_le16(0x0100);
      goto found_dfu;
    }
    libusb_free_config_descriptor(cfg);
    continue;

  found_dfu:
    if (func_dfu.bLength == 7) {
      printf("Deducing device DFU version from functional descriptor "
             "length\n");
      func_dfu.bcdDFUVersion = libusb_cpu_to_le16(0x0100);
    } else if (func_dfu.bLength < 9) {
      printf("Error obtaining DFU functional descriptor\n");
      printf("Please report this as a bug!\n");
      printf("Warning: Assuming DFU version 1.0\n");
      func_dfu.bcdDFUVersion = libusb_cpu_to_le16(0x0100);
      printf("Warning: Transfer size can not be detected\n");
      func_dfu.wTransferSize = 0;
    }

    for (intf_idx = 0; intf_idx < cfg->bNumInterfaces; intf_idx++) {
      int multiple_alt;

      if (match_iface_index > -1 && match_iface_index != intf_idx)
        continue;

      uif = &cfg->interface[intf_idx];
      if (!uif)
        break;

      multiple_alt = uif->num_altsetting > 0;

      for (alt_idx = 0; alt_idx < uif->num_altsetting; alt_idx++) {
        int dfu_mode;
        uint16_t quirks;

        quirks = get_quirks(desc->idVendor, desc->idProduct, desc->bcdDevice);

        intf = &uif->altsetting[alt_idx];

        /* DFU subclass */
        if (intf->bInterfaceClass != 0xfe || intf->bInterfaceSubClass != 1)
          continue;

        dfu_mode = (intf->bInterfaceProtocol == 2);

        /* ST DfuSe devices often use bInterfaceProtocol 0 instead of 2 */
        if (func_dfu.bcdDFUVersion == 0x011a && intf->bInterfaceProtocol == 0)
          dfu_mode = 1;

        /* LPC DFU bootloader has bInterfaceProtocol 1 (Runtime) instead of 2 */
        if (desc->idVendor == 0x1fc9 && desc->idProduct == 0x000c &&
            intf->bInterfaceProtocol == 1)
          dfu_mode = 1;

        /*
         * Old Jabra devices may have bInterfaceProtocol 0 instead of 2.
         * Also runtime PID and DFU pid are the same.
         * In DFU mode, the configuration descriptor has only 1 interface.
         */
        if (desc->idVendor == 0x0b0e && intf->bInterfaceProtocol == 0 &&
            cfg->bNumInterfaces == 1)
          dfu_mode = 1;

        if (dfu_mode && match_iface_alt_index > -1 &&
            match_iface_alt_index != intf->bAlternateSetting)
          continue;

        if (dfu_mode) {
          if ((match_vendor_dfu >= 0 && match_vendor_dfu != desc->idVendor) ||
              (match_product_dfu >= 0 &&
               match_product_dfu != desc->idProduct)) {
            continue;
          }
        } else {
          if ((match_vendor >= 0 && match_vendor != desc->idVendor) ||
              (match_product >= 0 && match_product != desc->idProduct)) {
            continue;
          }
        }

        if (match_devnum >= 0 && match_devnum != libusb_get_device_address(dev))
          continue;

        ret = libusb_open(dev, &devh);
        if (ret) {
          warnx("Cannot open DFU device %04x:%04x found on devnum %i (%s)",
                desc->idVendor, desc->idProduct, libusb_get_device_address(dev),
                libusb_error_name(ret));
          break;
        }
        if (intf->iInterface != 0)
          ret = get_string_descriptor_ascii(devh, intf->iInterface,
                                            (void *)alt_name, MAX_DESC_STR_LEN);
        else
          ret = -1;
        if (ret < 1)
          strcpy(alt_name, "UNKNOWN");
        if (desc->iSerialNumber != 0) {
          if (quirks & QUIRK_UTF8_SERIAL) {
            ret = get_utf8_string_descriptor(devh, desc->iSerialNumber,
                                             (void *)serial_name,
                                             MAX_DESC_STR_LEN - 1);
            if (ret >= 0)
              serial_name[ret] = '\0';
          } else {
            ret = get_string_descriptor_ascii(devh, desc->iSerialNumber,
                                              (void *)serial_name,
                                              MAX_DESC_STR_LEN);
          }
        } else {
          ret = -1;
        }
        if (ret < 1)
          strcpy(serial_name, "UNKNOWN");
        libusb_close(devh);

        if (dfu_mode && match_iface_alt_name != NULL &&
            strcmp(alt_name, match_iface_alt_name))
          continue;

        if (dfu_mode) {
          if (match_serial_dfu != NULL && strcmp(match_serial_dfu, serial_name))
            continue;
        } else {
          if (match_serial != NULL && strcmp(match_serial, serial_name))
            continue;
        }

        pdfu = dfu_malloc(sizeof(*pdfu));

        memset(pdfu, 0, sizeof(*pdfu));

        pdfu->func_dfu = func_dfu;
        pdfu->dev = libusb_ref_device(dev);
        pdfu->quirks = quirks;
        pdfu->vendor = desc->idVendor;
        pdfu->product = desc->idProduct;
        pdfu->bcdDevice = desc->bcdDevice;
        pdfu->configuration = cfg->bConfigurationValue;
        pdfu->interface = intf->bInterfaceNumber;
        pdfu->altsetting = intf->bAlternateSetting;
        pdfu->devnum = libusb_get_device_address(dev);
        pdfu->busnum = libusb_get_bus_number(dev);
        pdfu->alt_name = strdup(alt_name);
        if (pdfu->alt_name == NULL)
          errx(EX_SOFTWARE, "Out of memory");
        pdfu->serial_name = strdup(serial_name);
        if (pdfu->serial_name == NULL)
          errx(EX_SOFTWARE, "Out of memory");
        if (dfu_mode)
          pdfu->flags |= DFU_IFF_DFU;
        if (multiple_alt)
          pdfu->flags |= DFU_IFF_ALT;
        if (pdfu->quirks & QUIRK_FORCE_DFU11) {
          pdfu->func_dfu.bcdDFUVersion = libusb_cpu_to_le16(0x0110);
        }
        pdfu->bMaxPacketSize0 = desc->bMaxPacketSize0;

        /* append to list */
        if (!dfu_root) {
          dfu_root = pdfu;
        } else {
          struct dfu_if *last = dfu_root;
          while (last->next)
            last = last->next;
          last->next = pdfu;
        }
      }
    }
    libusb_free_config_descriptor(cfg);
  }
}

#define MAX_PATH_LEN 20
char path_buf[MAX_PATH_LEN];

char *get_path(libusb_device *dev) {
#if (defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000102) ||       \
    (defined(LIBUSBX_API_VERSION) && LIBUSBX_API_VERSION >= 0x01000102)
  uint8_t path[8];
  int r, j;
  r = libusb_get_port_numbers(dev, path, sizeof(path));
  if (r > 0) {
    sprintf(path_buf, "%d-%d", libusb_get_bus_number(dev), path[0]);
    for (j = 1; j < r; j++) {
      sprintf(path_buf + strlen(path_buf), ".%d", path[j]);
    };
  }
  return path_buf;
#else
#warning "libusb too old - building without USB path support!"
  (void)dev;
  return NULL;
#endif
}

void probe_devices(libusb_context *ctx) {
  libusb_device **list;
  ssize_t num_devs;
  ssize_t i;

  num_devs = libusb_get_device_list(ctx, &list);
  for (i = 0; i < num_devs; ++i) {
    struct libusb_device_descriptor desc;
    struct libusb_device *dev = list[i];

    if (match_path != NULL && strcmp(get_path(dev), match_path) != 0)
      continue;
    if (libusb_get_device_descriptor(dev, &desc))
      continue;
    probe_configuration(dev, &desc);
  }
  libusb_free_device_list(list, 1);
}

void disconnect_devices(void) {
  struct dfu_if *pdfu;
  struct dfu_if *prev = NULL;

  for (pdfu = dfu_root; pdfu != NULL; pdfu = pdfu->next) {
    free(prev);
    libusb_unref_device(pdfu->dev);
    free(pdfu->alt_name);
    free(pdfu->serial_name);
    prev = pdfu;
  }
  free(prev);
  dfu_root = NULL;
}

void print_dfu_if(struct dfu_if *dfu_if) {
  printf("Found %s: [%04x:%04x] ver=%04x, devnum=%u, cfg=%u, intf=%u, "
         "path=\"%s\", alt=%u, name=\"%s\", serial=\"%s\"\n",
         dfu_if->flags & DFU_IFF_DFU ? "DFU" : "Runtime", dfu_if->vendor,
         dfu_if->product, dfu_if->bcdDevice, dfu_if->devnum,
         dfu_if->configuration, dfu_if->interface, get_path(dfu_if->dev),
         dfu_if->altsetting, dfu_if->alt_name, dfu_if->serial_name);
}

/* Walk the device tree and print out DFU devices */
void list_dfu_interfaces(void) {
  struct dfu_if *pdfu;

  for (pdfu = dfu_root; pdfu != NULL; pdfu = pdfu->next)
    print_dfu_if(pdfu);
}
