/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stddef.h>
#include <dirent.h>
#include <ctype.h>

#include "efivars.h"

#define EFI_VENDOR_GLOBAL ((uint8_t[16]) { 0x8b,0xe4,0xdf,0x61,0x93,0xca,0x11,0xd2,0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c })

bool is_efi_boot(void) {
        return access("/sys/firmware/efi", F_OK) >= 0;
}

int efi_get_variable(
                const uint8_t vendor[16],
                const char *name,
                void **value,
                size_t *size) {

        int fd = -1;
        char *p = NULL;
        uint32_t a;
        ssize_t n;
        struct stat st;
        void *b;
        int r;

        assert(name);
        assert(value);
        assert(size);

        if (asprintf(&p,
                     "/sys/firmware/efi/efivars/%s-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     name,
                     vendor[0], vendor[1], vendor[2], vendor[3], vendor[4], vendor[5], vendor[6], vendor[7],
                     vendor[8], vendor[9], vendor[10], vendor[11], vendor[12], vendor[13], vendor[14], vendor[15]) < 0)
                return -ENOMEM;

        fd = open(p, O_RDONLY|O_NOCTTY|O_CLOEXEC);
        if (fd < 0) {
                r = -errno;
                goto finish;
        }

        if (fstat(fd, &st) < 0) {
                r = -errno;
                goto finish;
        }
        if (st.st_size < 4) {
                r = -EIO;
                goto finish;
        }
        if (st.st_size > 4*1024*1024 + 4) {
                r = -E2BIG;
                goto finish;
        }

        n = read(fd, &a, sizeof(a));
        if (n < 0) {
                r = errno;
                goto finish;
        }
        if (n != sizeof(a)) {
                r = -EIO;
                goto finish;
        }

        b = malloc(st.st_size - 4 + 2);
        if (!b) {
                r = -ENOMEM;
                goto finish;
        }

        n = read(fd, b, (size_t) st.st_size - 4);
        if (n < 0) {
                free(b);
                r = errno;
                goto finish;
        }
        if (n != (ssize_t) st.st_size - 4) {
                free(b);
                r = -EIO;
                goto finish;
        }

        /* Always NUL terminate (2 bytes, to protect UTF-16) */
        ((char*) b)[st.st_size - 4] = 0;
        ((char*) b)[st.st_size - 4 + 1] = 0;

        *value = b;
        *size = (size_t) st.st_size - 4;
        r = 0;

finish:
        if (fd >= 0)
                close(fd);

        free(p);
        return r;
}

int efi_get_variable_string(const uint8_t vendor[16], const char *name, char **p) {
        void *s = NULL;
        size_t ss;
        char *x;
        int r;

        r = efi_get_variable(vendor, name, &s, &ss);
        if (r < 0)
                return r;

        x = utf16_to_utf8(s, ss);
        free(s);
        if (!x)
                return -ENOMEM;

        *p = x;
        return 0;
}

static size_t utf16_size(const uint16_t *s) {
        size_t l = 0;

        while (s[l] > 0)
                l++;

        return (l+1) * sizeof(uint16_t);
}

static void efi_guid_to_id128(const void *guid, uint8_t *bytes) {
        struct uuid {
                uint32_t u1;
                uint16_t u2;
                uint16_t u3;
                uint8_t u4[8];
        } __attribute__((packed));
        const struct uuid *uuid = guid;

        bytes[0] = (uuid->u1 >> 24) & 0xff;
        bytes[1] = (uuid->u1 >> 16) & 0xff;
        bytes[2] = (uuid->u1 >> 8) & 0xff;
        bytes[3] = (uuid->u1) & 0xff;
        bytes[4] = (uuid->u2 >> 8) & 0xff;
        bytes[5] = (uuid->u2) & 0xff;
        bytes[6] = (uuid->u3 >> 8) & 0xff;
        bytes[7] = (uuid->u3) & 0xff;
        memcpy(bytes+8, uuid->u4, sizeof(uuid->u4));
}

static char *tilt_slashes(char *s) {
        char *p;

        for (p = s; *p; p++)
                if (*p == '\\')
                        *p = '/';

        return s;
}

int efi_get_boot_option(
                uint16_t id,
                char **title,
                uint8_t part_uuid[16],
                char **path) {

        struct boot_option {
                uint32_t attr;
                uint16_t path_len;
                uint16_t title[];
        } __attribute__((packed));

        struct drive_path {
                uint32_t part_nr;
                uint64_t part_start;
                uint64_t part_size;
                char signature[16];
                uint8_t mbr_type;
                uint8_t signature_type;
        } __attribute__((packed));

        struct device_path {
                uint8_t type;
                uint8_t sub_type;
                uint16_t length;
                union {
                        uint16_t path[0];
                        struct drive_path drive;
                };
        } __attribute__((packed));

        char boot_id[9];
        uint8_t *buf = NULL;
        size_t l;
        struct boot_option *header;
        size_t title_size;
        char *s = NULL;
        char *p = NULL;
        uint8_t p_uuid[16];
        int err;

        memset(p_uuid, 0, sizeof(p_uuid));

        snprintf(boot_id, sizeof(boot_id), "Boot%04X", id);
        err = efi_get_variable(EFI_VENDOR_GLOBAL, boot_id, (void **) &buf, &l);
        if (err < 0)
                return err;
        if (l < sizeof(struct boot_option)) {
                err = -ENOENT;
                goto err;
        }

        header = (struct boot_option *) buf;
        title_size = utf16_size(header->title);
        if (title_size > l - offsetof(struct boot_option, title)) {
                err = -EINVAL;
                goto err;
        }

        s = utf16_to_utf8(header->title, title_size);
        if (!s) {
                err = -ENOMEM;
                goto err;
        }

        if (header->path_len > 0) {
                uint8_t *dbuf;
                size_t dnext;

                dbuf = buf + offsetof(struct boot_option, title) + title_size;
                dnext = 0;
                while (dnext < header->path_len) {
                        struct device_path *dpath;

                        dpath = (struct device_path *)(dbuf + dnext);
                        if (dpath->length < 4)
                                break;

                        /* Type 0x7F – End of Hardware Device Path, Sub-Type 0xFF – End Entire Device Path */
                        if (dpath->type == 0x7f && dpath->sub_type == 0xff)
                                break;

                        dnext += dpath->length;

                        /* Type 0x04 – Media Device Path */
                        if (dpath->type != 0x04)
                                continue;

                        /* Sub-Type 1 – Hard Drive */
                        if (dpath->sub_type == 0x01) {
                                /* 0x02 – GUID Partition Table */
                                if (dpath->drive.mbr_type != 0x02)
                                        continue;

                                /* 0x02 – GUID signature */
                                if (dpath->drive.signature_type != 0x02)
                                        continue;

                                efi_guid_to_id128(dpath->drive.signature, p_uuid);
                                continue;
                        }

                        /* Sub-Type 4 – File Path */
                        if (dpath->sub_type == 0x04) {
                                p = utf16_to_utf8(dpath->path, dpath->length-4);
                                tilt_slashes(p);
                                continue;
                        }
                }
        }

        if (title)
                *title = s;
        if (part_uuid)
                memcpy(part_uuid, p_uuid, 16);
        if (path)
                *path = p;

        return 0;
err:
        free(s);
        free(p);
        free(buf);
        return err;
}

int efi_get_boot_order(uint16_t **order) {
        void *buf;
        size_t l;
        int r;

        r = efi_get_variable(EFI_VENDOR_GLOBAL, "BootOrder", &buf, &l);
        if (r < 0)
                return r;

        if (l <= 0) {
                free(buf);
                return -ENOENT;
        }

        if ((l % sizeof(uint16_t) > 0)) {
                free(buf);
                return -EINVAL;
        }

        *order = buf;
        return (int) (l / sizeof(uint16_t));
}

int efi_get_boot_options(uint16_t **options) {
        DIR *dir;
        struct dirent *de;
        uint16_t *list = NULL;
        int count = 0;

        assert(options);

        dir = opendir("/sys/firmware/efi/efivars/");
        if (!dir)
                return -errno;

        while ((de = readdir(dir))) {
                size_t n;
                int a, b, c, d;
                uint16_t *t;

                if (strncmp(de->d_name, "Boot", 4) != 0)
                        continue;

                n = strlen(de->d_name);
                if (n != 45)
                        continue;

                if (strcmp(de->d_name + 8, "-8be4df61-93ca-11d2-aa0d-00e098032b8c") != 0)
                        continue;

                a = de->d_name[4];
                b = de->d_name[5];
                c = de->d_name[6];
                d = de->d_name[7];

                if (!isdigit(a) || !isdigit(b) || !isdigit(c) || !isdigit(d))
                        continue;

                t = realloc(list, (count + 1) * sizeof(uint16_t));
                if (!t) {
                        free(list);
                        closedir(dir);
                        return -ENOMEM;
                }

                list = t;
                list[count ++] = (a - '0') * 1000 + (b - '0') * 100 + (c - '0') * 10 + (d - '0');

        }

        *options = list;
        return count;
}

char *utf16_to_utf8(const void *s, size_t length) {
        char *r;
        const uint8_t *f;
        uint8_t *t;

        r = malloc((length*3+1)/2 + 1);
        if (!r)
                return NULL;

        t = (uint8_t*) r;

        for (f = s; f < (const uint8_t*) s + length; f += 2) {
                uint16_t c;

                c = (f[1] << 8) | f[0];

                if (c == 0) {
                        *t = 0;
                        return r;
                } else if (c < 0x80) {
                        *(t++) = (uint8_t) c;
                } else if (c < 0x800) {
                        *(t++) = (uint8_t) (0xc0 | (c >> 6));
                        *(t++) = (uint8_t) (0x80 | (c & 0x3f));
                } else {
                        *(t++) = (uint8_t) (0xe0 | (c >> 12));
                        *(t++) = (uint8_t) (0x80 | ((c >> 6) & 0x3f));
                        *(t++) = (uint8_t) (0x80 | (c & 0x3f));
                }
        }

        *t = 0;

        return r;
}
