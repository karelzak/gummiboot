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

#include <stdio.h>
#include <getopt.h>
#include <errno.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/statfs.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>
#include <ftw.h>
#include <stdbool.h>
#include <blkid.h>

#include "efivars.h"

#define ELEMENTSOF(x) (sizeof(x)/sizeof((x)[0]))
#define streq(a,b) (strcmp((a),(b)) == 0)

static inline bool isempty(const char *p) {
        return !p || !p[0];
}

static inline const char *strna(const char *s) {
        return isempty(s) ? "n/a" : s;
}

/* TODO:
 *
 * - Generate loader.conf from /etc/os-release?
 */

static int help(void) {
        printf("%s [COMMAND] [OPTIONS...]\n"
               "\n"
               "Install, update or remove the Gummiboot EFI boot loader.\n\n"
               "  -h --help          Show this help\n"
               "     --path=PATH     Path to the EFI System Partition (ESP)\n"
               "     --no-variables  Don't touch EFI variables\n"
               "\n"
               "Comands:\n"
               "     install         Install Gummiboot to the ESP and EFI variables\n"
               "     update          Update Gummiboot in the ESP and EFI variables\n"
               "     remove          Remove Gummiboot from the ESP and EFI variables\n",
               program_invocation_short_name);

        return 0;
}

static int uuid_parse(const char *s, uint8_t uuid[16]) {
        int u[16];
        int i;

        if (sscanf(s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            &u[0], &u[1], &u[2], &u[3], &u[4], &u[5], &u[6], &u[7],
            &u[8], &u[9], &u[10], &u[11], &u[12], &u[13], &u[14], &u[15]) != 16)
                return -EINVAL;

        for (i = 0; i < 16; i++)
                uuid[i] = u[i];

        return 0;
}

static int verify_esp(const char *p, uint32_t *part, uint64_t *pstart, uint64_t *psize, uint8_t uuid[16]) {
        struct statfs sfs;
        struct stat st, st2;
        char *t;
        blkid_probe b = NULL;
        int r;
        const char *v;

        if (statfs(p, &sfs) < 0) {
                fprintf(stderr, "Failed to check file system type of %s: %m\n", p);
                return -errno;
        }

        if (sfs.f_type != 0x4d44) {
                fprintf(stderr, "File system %s is not a FAT EFI System Partition (ESP) file system.\n", p);
                return -ENODEV;
        }

        if (stat(p, &st) < 0) {
                fprintf(stderr, "Failed to determine block device node of %s: %m\n", p);
                return -errno;
        }

        if (major(st.st_dev) == 0) {
                fprintf(stderr, "Block device node of %p is invalid.\n", p);
                return -ENODEV;
        }

        r = asprintf(&t, "%s/..", p);
        if (r < 0) {
                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        r = stat(t, &st2);
        free(t);
        if (r < 0) {
                fprintf(stderr, "Failed to determine block device node of parent of %s: %m\n", p);
                return -errno;
        }

        if (st.st_dev == st2.st_dev) {
                fprintf(stderr, "Directory %s is not the root of the EFI System Partition (ESP) file system.\n", p);
                return -ENODEV;
        }

        r = asprintf(&t, "/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
        if (r < 0) {
                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        errno = 0;
        b = blkid_new_probe_from_filename(t);
        free(t);
        if (!b) {
                if (errno != 0) {
                        fprintf(stderr, "Failed to open file system %s: %s\n", p, strerror(errno));
                        return -errno;
                }

                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        blkid_probe_enable_superblocks(b, 1);
        blkid_probe_set_superblocks_flags(b, BLKID_SUBLKS_TYPE);
        blkid_probe_enable_partitions(b, 1);
        blkid_probe_set_partitions_flags(b, BLKID_PARTS_ENTRY_DETAILS);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == -2) {
                fprintf(stderr, "File system %s is ambigious.\n", p);
                r = -ENODEV;
                goto fail;
        } else if (r == 1) {
                fprintf(stderr, "File system %s does not contain a label.\n", p);
                r = -ENODEV;
                goto fail;
        } else if (r != 0) {
                fprintf(stderr, "Failed to probe file system %s: %s\n", p, strerror(errno ? errno : EIO));
                r = errno ? -errno : -EIO;
                goto fail;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "TYPE", &v, NULL);
        if (r != 0) {
                fprintf(stderr, "Failed to probe file system type %s: %s\n", p, strerror(errno ? errno : EIO));
                r = errno ? -errno : -EIO;
                goto fail;
        }

        if (strcmp(v, "vfat") != 0) {
                fprintf(stderr, "File system %s is not a FAT EFI System Partition (ESP) file system after all.\n", p);
                r = -ENODEV;
                goto fail;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_SCHEME", &v, NULL);
        if (r != 0) {
                fprintf(stderr, "Failed to probe partition scheme %s: %s\n", p, strerror(errno ? errno : EIO));
                r = errno ? -errno : -EIO;
                goto fail;
        }

        if (strcmp(v, "gpt") != 0) {
                fprintf(stderr, "File system %s is not on a GPT partition table.\n", p);
                r = -ENODEV;
                goto fail;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_TYPE", &v, NULL);
        if (r != 0) {
                fprintf(stderr, "Failed to probe partition type UUID %s: %s\n", p, strerror(errno ? errno : EIO));
                r = errno ? -errno : -EIO;
                goto fail;
        }

        if (strcmp(v, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") != 0) {
                fprintf(stderr, "File system %s is not an EFI System Partition (ESP).\n", p);
                r = -ENODEV;
                goto fail;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_UUID", &v, NULL);
        if (r != 0) {
                fprintf(stderr, "Failed to probe partition entry UUID %s: %s\n", p, strerror(errno ? errno : EIO));
                r = errno ? -errno : -EIO;
                goto fail;
        }
        uuid_parse(v, uuid);

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_NUMBER", &v, NULL);
        if (r != 0) {
                fprintf(stderr, "Failed to probe partition number %s: %s\n", p, strerror(errno ? errno : EIO));
                r = errno ? -errno : -EIO;
                goto fail;
        }
        *part = strtoul(v, NULL, 10);

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_OFFSET", &v, NULL);
        if (r != 0) {
                fprintf(stderr, "Failed to probe partition offset %s: %s\n", p, strerror(errno ? errno : EIO));
                r = errno ? -errno : -EIO;
                goto fail;
        }
        *pstart = strtoul(v, NULL, 10);

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_SIZE", &v, NULL);
        if (r != 0) {
                fprintf(stderr, "Failed to probe partition size %s: %s\n", p, strerror(errno ? errno : EIO));
                r = errno ? -errno : -EIO;
                goto fail;
        }
        *psize = strtoul(v, NULL, 10);

        blkid_free_probe(b);
        return 0;
fail:
        if (b)
                blkid_free_probe(b);
        return r;
}

/* search for "#### LoaderInfo: gummiboot 31 ####" string inside the binary */
static int get_file_version(FILE *f, char **v) {
        struct stat st;
        char *buf;
        const char *s, *e;
        char *x = NULL;
        int r = 0;

        assert(f);
        assert(v);

        if (fstat(fileno(f), &st) < 0)
                return -errno;

        if (st.st_size < 27)
                return 0;

        buf = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fileno(f), 0);
        if (buf == MAP_FAILED)
                return -errno;

        s = memmem(buf, st.st_size - 8, "#### LoaderInfo: ", 17);
        if (!s)
                goto finish;
        s += 17;

        e = memmem(s, st.st_size - (s - buf), " ####", 5);
        if (!e || e - s < 3) {
                fprintf(stderr, "Malformed version string.\n");
                r = -EINVAL;
                goto finish;
        }

        x = strndup(s, e - s);
        if (!x) {
                fprintf(stderr, "Out of memory.\n");
                r = -ENOMEM;
                goto finish;
        }
        r = 1;

finish:
        munmap(buf, st.st_size);
        *v = x;
        return r;
}

static int enumerate_binaries(const char *esp_path, const char *path, const char *prefix) {
        struct dirent *de;
        char *p = NULL, *q = NULL;
        DIR *d = NULL;
        int r = 0, c = 0;

        if (asprintf(&p, "%s/%s", esp_path, path) < 0) {
                fprintf(stderr, "Out of memory.\n");
                r = -ENOMEM;
                goto finish;
        }

        d = opendir(p);
        if (!d) {
                if (errno == ENOENT) {
                        r = 0;
                        goto finish;
                }

                fprintf(stderr, "Failed to read %s: %m\n", p);
                r = -errno;
                goto finish;
        }

        while ((de = readdir(d))) {
                char *v;
                size_t n;
                FILE *f;

                if (de->d_name[0] == '.')
                        continue;

                n = strlen(de->d_name);
                if (n < 4 || strcasecmp(de->d_name + n - 4, ".efi") != 0)
                        continue;

                if (prefix && strncasecmp(de->d_name, prefix, strlen(prefix)) != 0)
                        continue;

                free(q);
                q = NULL;
                if (asprintf(&q, "%s/%s/%s", esp_path, path, de->d_name) < 0) {
                        fprintf(stderr, "Out of memory.\n");
                        r = -ENOMEM;
                        goto finish;
                }

                f = fopen(q, "re");
                if (!f) {
                        fprintf(stderr, "Failed to open %s for reading: %m\n", q);
                        r = -errno;
                        goto finish;
                }

                r = get_file_version(f, &v);
                fclose(f);

                if (r < 0)
                        goto finish;

                if (r == 0)
                        printf("\t%s (Unknown product and version)\n", q);
                else
                        printf("\t%s (%s)\n", q, v);

                c++;

                free(v);
        }

        r = c;

finish:
        if (d)
                closedir(d);

        free(p);
        free(q);

        return r;
}

static int status_binaries(const char *esp_path) {
        int r;

        printf("Boot Loader Binaries found in ESP:\n");

        r = enumerate_binaries(esp_path, "EFI/gummiboot", NULL);
        if (r == 0)
                fprintf(stderr, "\tGummiboot not installed in ESP.\n");
        else if (r < 0)
                return r;

        r = enumerate_binaries(esp_path, "EFI/BOOT", "BOOT");
        if (r == 0)
                fprintf(stderr, "\tNo fallback for removable devices installed in ESP.\n");
        else if (r < 0)
                return r;

        return 0;
}

static int print_efi_option(uint16_t id, bool in_order) {
        char *title = NULL;
        char *path = NULL;
        uint8_t partition[16];
        int r = 0;

        r = efi_get_boot_option(id, &title, partition, &path);
        if (r < 0) {
                fprintf(stderr, "Failed to read EFI boot entry %i.\n", id);
                goto finish;
        }

        printf("\t%s%s\n", strna(title), in_order ? " [ENABLED]" : "");
        if (path) {
                 printf("\t\t%s\n", path);
                 printf("\t\t/dev/disk/by-partuuid/%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                        partition[0], partition[1], partition[2], partition[3], partition[4], partition[5], partition[6], partition[7],
                        partition[8], partition[9], partition[10], partition[11], partition[12], partition[13], partition[14], partition[15]);
        }

finish:
        free(title);
        free(path);
        return r;
}

static int status_variables(void) {
        int n_options, n_order;
        uint16_t *options = NULL, *order = NULL;
        int r, i;

        if (!is_efi_boot()) {
                fprintf(stderr, "Not booted with EFI, not showing EFI variables.\n");
                return 0;
        }

        printf("\nBoot Entries found in EFI variables:\n");

        n_options = efi_get_boot_options(&options);
        if (n_options < 0) {
                if (n_options == -ENOENT)
                        fprintf(stderr, "\tFailed to access EFI variables. Is the \"efivarfs\" filesystem mounted?\n");
                else
                        fprintf(stderr, "\tFailed to read EFI boot entries.\n");
                r = n_options;
                goto finish;
        }

        n_order = efi_get_boot_order(&order);
        if (n_order == -ENOENT) {
                fprintf(stderr, "\tNo boot entries registered in EFI variables.\n");
                r = 0;
                goto finish;
        } else if (n_order < 0) {
                fprintf(stderr, "\tFailed to read EFI boot order.\n");
                r = n_order;
                goto finish;
        }

        for (i = 0; i < n_order; i++) {
                r = print_efi_option(order[i], true);
                if (r < 0)
                        goto finish;
        }

        for (i = 0; i < n_options; i++) {
                int j;
                bool found = false;

                for (j = 0; j < n_order; j++)
                        if (options[i] == order[j]) {
                                found = true;
                                break;
                        }

                if (found)
                        continue;

                r = print_efi_option(options[i], false);
                if (r < 0)
                        goto finish;
        }

        r = 0;
finish:
        free(options);
        free(order);

        return r;
}

static int compare_product(const char *a, const char *b) {
        size_t x, y;

        assert(a);
        assert(b);

        x = strcspn(a, " ");
        y = strcspn(b, " ");
        if (x != y)
                return x < y ? -1 : x > y ? 1 : 0;

        return strncmp(a, b, x);
}

static int compare_version(const char *a, const char *b) {
        assert(a);
        assert(b);

        a += strcspn(a, " ");
        a += strspn(a, " ");
        b += strcspn(b, " ");
        b += strspn(b, " ");

        return strverscmp(a, b);
}

static int version_check(FILE *f, const char *from, const char *to) {
        FILE *g = NULL;
        char *a = NULL, *b = NULL;
        int r;

        assert(f);
        assert(from);
        assert(to);

        r = get_file_version(f, &a);
        if (r < 0)
                goto finish;
        if (r == 0) {
                r = -EINVAL;
                fprintf(stderr, "Source file %s does not carry version information!\n", from);
                goto finish;
        }

        g = fopen(to, "re");
        if (!g) {
                if (errno == ENOENT) {
                        r = 0;
                        goto finish;
                }

                r = -errno;
                fprintf(stderr, "Failed to open %s for reading: %m\n", to);
                goto finish;
        }

        r = get_file_version(g, &b);
        if (r < 0)
                goto finish;
        if (r == 0 || compare_product(a, b) != 0) {
                r = -EEXIST;
                fprintf(stderr, "Skipping %s, since it's owned by another boot loader.\n", to);
                goto finish;
        }

        if (compare_version(a, b) < 0) {
                r = -EEXIST;
                fprintf(stderr, "Skipping %s, since it's a newer boot loader version already.\n", to);
                goto finish;
        }

        r = 0;

finish:
        free(a);
        free(b);
        if (g)
                fclose(g);
        return r;
}

static int copy_file(const char *from, const char *to, bool force) {
        FILE *f = NULL, *g = NULL;
        char *p = NULL;
        int r;
        struct timespec t[2];
        struct stat st;

        assert(from);
        assert(to);

        f = fopen(from, "re");
        if (!f) {
                fprintf(stderr, "Failed to open %s for reading: %m\n", from);
                return -errno;
        }

        if (!force) {
                /* If this is an update, then let's compare versions first */
                r = version_check(f, from, to);
                if (r < 0)
                        goto finish;
        }

        if (asprintf(&p, "%s~", to) < 0) {
                fprintf(stderr, "Out of memory.\n");
                r = -ENOMEM;
                goto finish;
        }

        g = fopen(p, "wxe");
        if (!g) {
                /* Directory doesn't exist yet? Then let's skip this... */
                if (!force && errno == ENOENT) {
                        r = 0;
                        goto finish;
                }

                fprintf(stderr, "Failed to open %s for writing: %m\n", to);
                r = -errno;
                goto finish;
        }

        rewind(f);
        do {
                size_t k;
                uint8_t buf[32*1024];

                k = fread(buf, 1, sizeof(buf), f);
                if (ferror(f)) {
                        fprintf(stderr, "Failed to read %s: %m\n", from);
                        r = -errno;
                        goto finish;
                }
                if (k == 0)
                        break;

                fwrite(buf, 1, k, g);
                if (ferror(g)) {
                        fprintf(stderr, "Failed to write %s: %m\n", to);
                        r = -errno;
                        goto finish;
                }
        } while (!feof(f));

        fflush(g);
        if (ferror(g)) {
                fprintf(stderr, "Failed to write %s: %m\n", to);
                r = -errno;
                goto finish;
        }

        r = fstat(fileno(f), &st);
        if (r < 0) {
                fprintf(stderr, "Failed to get file timestamps of %s: %m", from);
                r = -errno;
                goto finish;
        }

        t[0] = st.st_atim;
        t[1] = st.st_mtim;

        r = futimens(fileno(g), t);
        if (r < 0) {
                fprintf(stderr, "Failed to change file timestamps for %s: %m", p);
                r = -errno;
                goto finish;
        }

        if (rename(p, to) < 0) {
                fprintf(stderr, "Failed to rename %s to %s: %m\n", p, to);
                r = -errno;
                goto finish;
        }

        fprintf(stderr, "Copied %s to %s.\n", from, to);

        free(p);
        p = NULL;
        r = 0;

finish:
        if (f)
                fclose(f);
        if (g)
                fclose(g);
        if (p) {
                unlink(p);
                free(p);
        }
        return r;
}

static char* strupper(char *s) {
        char *p;

        for (p = s; *p; p++)
                *p = toupper(*p);

        return s;
}

static int mkdir_one(const char *prefix, const char *suffix) {
        char *p;

        if (asprintf(&p, "%s/%s", prefix, suffix) < 0) {
                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        if (mkdir(p, 0700) < 0) {
                if (errno != EEXIST) {
                        fprintf(stderr, "Failed to create %s: %m\n", p);
                        free(p);
                        return -errno;
                }
        } else
                fprintf(stderr, "Created %s.\n", p);

        free(p);
        return 0;
}

static int create_dirs(const char *esp_path) {
        int r;

        r = mkdir_one(esp_path, "EFI");
        if (r < 0)
                return r;

        r = mkdir_one(esp_path, "EFI/gummiboot");
        if (r < 0)
                return r;

        r = mkdir_one(esp_path, "EFI/BOOT");
        if (r < 0)
                return r;

        r = mkdir_one(esp_path, "loader");
        if (r < 0)
                return r;

        r = mkdir_one(esp_path, "loader/entries");
        if (r < 0)
                return r;

        return 0;
}

static int copy_one_file(const char *esp_path, const char *name, bool force) {
        char *p = NULL, *q = NULL, *v = NULL;
        int r;

        if (asprintf(&p, "/usr/lib/gummiboot/%s", name) < 0) {
                fprintf(stderr, "Out of memory.\n");
                r = -ENOMEM;
                goto finish;
        }

        if (asprintf(&q, "%s/EFI/gummiboot/%s", esp_path, name) < 0) {
                fprintf(stderr, "Out of memory.\n");
                r = -ENOMEM;
                goto finish;
        }

        r = copy_file(p, q, force);

        if (strncmp(name, "gummiboot", 9) == 0) {
                int k;

                /* Create the fallback names for removable devices */
                if (asprintf(&v, "%s/EFI/BOOT/%s", esp_path, name + 5) < 0) {
                        fprintf(stderr, "Out of memory.\n");
                        r = -ENOMEM;
                        goto finish;
                }
                strupper(strrchr(v, '/') + 1);

                k = copy_file(p, v, force);
                if (k < 0 && r == 0) {
                        r = k;
                        goto finish;
                }
        }

finish:
        free(p);
        free(q);
        free(v);
        return r;
}

static int install_binaries(const char *esp_path, bool force) {
        struct dirent *de;
        DIR *d;
        int r = 0;

        if (force) {
                /* Don't create any of these directories when we are
                 * just updating. When we update we'll drop-in our
                 * files (unless there are newer ones already), but we
                 * won't create the directories for them in the first
                 * place. */
                r = create_dirs(esp_path);
                if (r < 0)
                        return r;
        }

        d = opendir("/usr/lib/gummiboot");
        if (!d) {
                fprintf(stderr, "Failed to open /usr/lib/gummiboot: %s\n", strerror(errno));
                return -errno;
        }

        while ((de = readdir(d))) {
                size_t n;
                int k;

                if (de->d_name[0] == '.')
                        continue;

                n = strlen(de->d_name);
                if (n < 4 || strcmp(de->d_name + n - 4, ".efi") != 0)
                        continue;

                k = copy_one_file(esp_path, de->d_name, force);
                if (k < 0 && r == 0)
                        r = k;
        }

        closedir(d);
        return r;
}

static bool same_entry(uint16_t id, const uint8_t uuid[16], const char *path) {
        char *opath = NULL;
        uint8_t ouuid[16];
        int err;
        bool same = false;

        err = efi_get_boot_option(id, NULL, ouuid, &opath);
        if (err < 0)
                return false;
        if (memcmp(uuid, ouuid, 16) != 0)
                goto finish;

        if (!streq(path, opath))
                goto finish;

        same = true;

finish:
        free(opath);
        return same;
}

static int find_slot(const uint8_t uuid[16], const char *path, uint16_t *id) {
        uint16_t *options = NULL;
        int n_options;
        int i;
        uint16_t new_id = 0;
        bool existing = false;

        n_options = efi_get_boot_options(&options);
        if (n_options < 0)
                return n_options;

        /* find already existing gummiboot entry */
        for (i = 0; i < n_options; i++)
                if (same_entry(options[i], uuid, path)) {
                        new_id = i;
                        existing = true;
                        goto finish;
                }

        /* find free slot in the sorted BootXXXX variable list */
        for (i = 0; i < n_options; i++)
                if (i != options[i]) {
                        new_id = i;
                        goto finish;
                }

finish:
        *id = new_id;
        free(options);
        return existing;
}

static int insert_into_order(uint16_t slot, bool first) {
        uint16_t *order = NULL;
        uint16_t *new_order;
        int n_order;
        int i;
        int err = 0;

        n_order = efi_get_boot_order(&order);
        if (n_order <= 0) {
                err = efi_set_boot_order(&slot, 1);
                goto finish;
        }

        /* are we already in the boot order? */
        for (i = 0; i < n_order; i++)
                if (order[i] == slot)
                        goto finish;

        /* extend array */
        new_order = realloc(order, (n_order+1) * sizeof(uint16_t));
        if (!new_order) {
                err = -ENOMEM;
                goto finish;
        }
        order = new_order;

        /* add to the top or end of the list */
        if (first) {
                memmove(&order[1], order, n_order * sizeof(uint16_t));
                order[0] = slot;
        } else
                order[n_order] = slot;

        efi_set_boot_order(order, n_order+1);

finish:
        free(order);
        return err;
}

static int remove_from_order(uint16_t slot) {
        uint16_t *order = NULL;
        int n_order;
        int i;
        int err = 0;

        n_order = efi_get_boot_order(&order);
        if (n_order < 0)
                return n_order;
        if (n_order == 0)
                return 0;

        for (i = 0; i < n_order; i++) {
                if (order[i] != slot)
                        continue;

                if (i+1 < n_order)
                        memmove(&order[i], &order[i+1], (n_order - i) * sizeof(uint16_t));
                efi_set_boot_order(order, n_order-1);
                break;
        }

        free(order);
        return err;
}

static int install_variables(const char *esp_path,
                             uint32_t part, uint64_t pstart, uint64_t psize,
                             const uint8_t uuid[16], const char *path,
                             bool force) {
        char *p = NULL;
        uint16_t *options = NULL;
        uint16_t slot;
        int err;

        if (!is_efi_boot()) {
                fprintf(stderr, "Not booted with EFI, skipping EFI variable checks.\n");
                return 0;
        }

        if (asprintf(&p, "%s%s", esp_path, path) < 0) {
                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        if (access(p, F_OK) < 0) {
                if (errno == ENOENT)
                        err = 0;
                else
                        err = -errno;
                goto finish;
        }

        err = find_slot(uuid, path, &slot);
        if (err < 0) {
                if (err == -ENOENT)
                        fprintf(stderr, "Failed to access EFI variables. Is the \"efivarfs\" filesystem mounted?\n");
                else
                        fprintf(stderr, "Failed to determine current boot order: %s\n", strerror(err));
                goto finish;
        }

        if (force || err == false) {
                err = efi_add_boot_option(slot,
                                          "Linux Boot Manager",
                                          part, pstart, psize,
                                          uuid, path);
                if (err < 0) {
                        fprintf(stderr, "Failed to create EFI Boot variable entry: %s\n", strerror(err));
                        goto finish;
                }
                fprintf(stderr, "Created EFI Boot entry \"Linux Boot Manager\".\n");
        }
        insert_into_order(slot, force);

finish:
        free(p);
        free(options);
        return err;
}

static int delete_nftw(const char *path, const struct stat *sb, int typeflag, struct FTW *ftw) {
        int r;

        if (typeflag == FTW_D || typeflag == FTW_DNR || typeflag == FTW_DP)
                r = rmdir(path);
        else
                r = unlink(path);

        if (r < 0)
                fprintf(stderr, "Failed to remove %s: %m\n", path);
        else
                fprintf(stderr, "Removed %s.\n", path);

        return 0;
}

static int rm_rf(const char *p) {
        nftw(p, delete_nftw, 20, FTW_DEPTH|FTW_MOUNT|FTW_PHYS);
        return 0;
}

static int remove_boot_efi(const char *esp_path) {
        struct dirent *de;
        char *p = NULL, *q = NULL;
        DIR *d = NULL;
        int r = 0, c = 0;

        if (asprintf(&p, "%s/EFI/BOOT", esp_path) < 0) {
                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        d = opendir(p);
        if (!d) {
                if (errno == ENOENT) {
                        r = 0;
                        goto finish;
                }

                fprintf(stderr, "Failed to read %s: %m\n", p);
                r = -errno;
                goto finish;
        }

        while ((de = readdir(d))) {
                char *v;
                size_t n;
                FILE *f;

                if (de->d_name[0] == '.')
                        continue;

                n = strlen(de->d_name);
                if (n < 4 || strcasecmp(de->d_name + n - 4, ".EFI") != 0)
                        continue;

                if (strncasecmp(de->d_name, "BOOT", 4) != 0)
                        continue;

                free(q);
                q = NULL;
                if (asprintf(&q, "%s/%s", p, de->d_name) < 0) {
                        fprintf(stderr, "Out of memory.\n");
                        r = -ENOMEM;
                        goto finish;
                }

                f = fopen(q, "re");
                if (!f) {
                        fprintf(stderr, "Failed to open %s for reading: %m\n", q);
                        r = -errno;
                        goto finish;
                }

                r = get_file_version(f, &v);
                fclose(f);

                if (r < 0)
                        goto finish;

                if (r > 0 && strncmp(v, "gummiboot ", 10) == 0) {

                        r = unlink(q);
                        if (r < 0) {
                                fprintf(stderr, "Failed to remove %s: %m\n", q);
                                r = -errno;
                                free(v);
                                goto finish;
                        } else
                                fprintf(stderr, "Removed %s.\n", q);
                }

                c++;
                free(v);
        }

        r = c;

finish:
        if (d)
                closedir(d);
        free(p);
        free(q);

        return r;
}

static int rmdir_one(const char *prefix, const char *suffix) {
        char *p;

        if (asprintf(&p, "%s/%s", prefix, suffix) < 0) {
                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        if (rmdir(p) < 0) {
                if (errno != ENOENT && errno != ENOTEMPTY) {
                        fprintf(stderr, "Failed to remove %s: %m\n", p);
                        free(p);
                        return -errno;
                }
        } else
                fprintf(stderr, "Removed %s.\n", p);

        free(p);
        return 0;
}


static int remove_binaries(const char *esp_path) {
        char *p;
        int r, q;

        if (asprintf(&p, "%s/EFI/gummiboot", esp_path) < 0) {
                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        r = rm_rf(p);
        free(p);

        q = remove_boot_efi(esp_path);
        if (q < 0 && r == 0)
                r = q;

        q = rmdir_one(esp_path, "loader/entries");
        if (q < 0 && r == 0)
                r = q;

        q = rmdir_one(esp_path, "loader");
        if (q < 0 && r == 0)
                r = q;

        q = rmdir_one(esp_path, "EFI/BOOT");
        if (q < 0 && r == 0)
                r = q;

        q = rmdir_one(esp_path, "EFI/gummiboot");
        if (q < 0 && r == 0)
                r = q;

        q = rmdir_one(esp_path, "EFI");
        if (q < 0 && r == 0)
                r = q;

        return r;
}

static int remove_variables(const uint8_t uuid[16], const char *path, bool in_order) {
        uint16_t slot;
        int err;

        if (!is_efi_boot())
                return 0;

        err = find_slot(uuid, path, &slot);
        if (err != 1)
                return 0;

        err = efi_remove_boot_option(slot);
        if (err < 0)
                return err;

        if (in_order)
                remove_from_order(slot);

        return 0;
}

static int install_loader_config(const char *esp_path) {
        char *p = NULL;
        char line[LINE_MAX];
        char *vendor = NULL;
        FILE *f;

        f = fopen("/etc/os-release", "re");
        if (!f)
                return -errno;

        while (fgets(line, sizeof(line), f) != NULL) {
                char *s;

                if (strncmp(line, "ID=", 3) != 0)
                        continue;
                vendor = line + 3;
                s = strchr(vendor, '\n');
                if (s)
                        s[0] = '\0';
                break;
        }

        fclose(f);

        if (!vendor)
                return -ESRCH;

        if (asprintf(&p, "%s/%s", esp_path, "loader/loader.conf") < 0) {
                fprintf(stderr, "Out of memory.\n");
                return -ENOMEM;
        }

        f = fopen(p, "wxe");
        if (f) {
                fprintf(f, "#timeout 3\n");
                fprintf(f, "default %s-*\n", vendor);
                fclose(f);
        }

        free(p);
        return 0;
}

static const char *arg_path = NULL;
static bool arg_touch_variables = true;

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_PATH = 0x100,
                ARG_NO_VARIABLES
        };

        static const struct option options[] = {
                { "help",         no_argument,       NULL, 'h'              },
                { "path",         required_argument, NULL, ARG_PATH         },
                { "no-variables", no_argument,       NULL, ARG_NO_VARIABLES },
                { NULL,           0,                 NULL, 0                }
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {
                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_PATH:
                        arg_path = optarg;
                        break;

                case ARG_NO_VARIABLES:
                        arg_touch_variables = false;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        fprintf(stderr, "Unknown option code '%c'.\n", c);
                        return -EINVAL;
                }
        }

        return 1;
}

int main(int argc, char*argv[]) {
        enum action {
                ACTION_STATUS,
                ACTION_INSTALL,
                ACTION_UPDATE,
                ACTION_REMOVE
        } arg_action = ACTION_STATUS;

        static const struct {
                const char* verb;
                enum action action;
        } verbs[] = {
                { "status",  ACTION_STATUS },
                { "install", ACTION_INSTALL },
                { "update",  ACTION_UPDATE },
                { "remove",  ACTION_REMOVE },
        };

        uint8_t uuid[16] = "";
        uint32_t part = 0;
        uint64_t pstart = 0;
        uint64_t psize = 0;
        unsigned int i;
        int r, q;

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        if (argv[optind]) {
                for (i = 0; i < ELEMENTSOF(verbs); i++) {
                        if (!streq(argv[optind], verbs[i].verb))
                                continue;
                        arg_action = verbs[i].action;
                        break;
                }
                if (i >= ELEMENTSOF(verbs)) {
                        fprintf(stderr, "Unknown operation %s\n", argv[optind]);
                        r = -EINVAL;
                        goto finish;
                }
        }

        if (!arg_path)
                arg_path = "/boot";

        if (geteuid() != 0) {
                fprintf(stderr, "Need to be root.\n");
                r = -EPERM;
                goto finish;
        }

        r = verify_esp(arg_path, &part, &pstart, &psize, uuid);
        if (r == -ENODEV && !arg_path)
                fprintf(stderr, "You might want to use --path= to indicate the path to your ESP, in case it is not mounted to /boot.\n");
        if (r < 0)
                goto finish;

        switch (arg_action) {
        case ACTION_STATUS:
                r = status_binaries(arg_path);
                if (r < 0)
                        goto finish;

                if (arg_touch_variables)
                        r = status_variables();
                break;

        case ACTION_INSTALL:
        case ACTION_UPDATE:
                umask(0002);

                r = install_binaries(arg_path, arg_action == ACTION_INSTALL);
                if (r < 0)
                        goto finish;

                if (arg_action == ACTION_INSTALL)
                        install_loader_config(arg_path);

                if (arg_touch_variables)
                        r = install_variables(arg_path,
                                              part, pstart, psize, uuid,
                                              "/EFI/gummiboot/gummiboot" MACHINE_TYPE_NAME ".efi",
                                              arg_action == ACTION_INSTALL);
                break;

        case ACTION_REMOVE:
                r = remove_binaries(arg_path);

                if (arg_touch_variables) {
                        q = remove_variables(uuid, "/EFI/gummiboot/gummiboot" MACHINE_TYPE_NAME ".efi", true);
                        if (q < 0 && r == 0)
                                r = q;
                }
                break;
        }

finish:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
