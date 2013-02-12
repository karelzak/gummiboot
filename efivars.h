/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#pragma once

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

#include <stdbool.h>
#include <sys/types.h>
#include <inttypes.h>

bool is_efi_boot(void);
int efi_get_variable(const uint8_t vendor[16], const char *name, void **value, size_t *size);
int efi_get_variable_string(const uint8_t vendor[16], const char *name, char **p);
int efi_get_boot_option(uint16_t id, char **title, uint8_t part_uuid[16], char **path);
int efi_get_boot_options(uint16_t **options);
int efi_get_boot_order(uint16_t **order);

char *utf16_to_utf8(const void *s, size_t length);
