/*
 * Simple UEFI boot loader which executes configured EFI images, where the
 * default entry is selected by a configured pattern (glob) or an on-screen
 * menu.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * Copyright (C) 2012 Kay Sievers <kay.sievers@vrfy.org>
 * Copyright (C) 2012 Harald Hoyer <harald@redhat.com>
 *
 * "Any intelligent fool can make things bigger, more complex, and more violent."
 *   -- Albert Einstein
 */

#include "efi.h"
#include "efilib.h"

/*
 * Allocated random UUID, intended to be shared across tools that implement
 * the (ESP)\loader\entries\<vendor>-<revision>.conf convention and the
 * associated EFI variables.
 */
static const EFI_GUID loader_guid = { 0x4a67b082, 0x0a4c, 0x41cf, {0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f} };

enum loader_type {
        LOADER_UNDEFINED,
        LOADER_EFI,
        LOADER_LINUX
};

typedef struct {
        CHAR16 *file;
        CHAR16 *title;
        enum loader_type type;
        CHAR16 *loader;
        CHAR16 *initrd;
        CHAR16 *options;
        BOOLEAN no_default;
} ConfigEntry;

typedef struct {
        ConfigEntry **entries;
        UINTN entry_count;
        UINTN idx_default;
        INTN idx_default_efivar;
        UINTN timeout_sec;
        UINTN timeout_sec_config;
        INTN timeout_sec_efivar;
        CHAR16 *entry_default_pattern;
        CHAR16 *options_edit;
        BOOLEAN no_initrd;
} Config;

#ifdef __x86_64__
static UINT64 tsc() {
        UINT64 a, d;
        __asm__ volatile ("rdtsc" : "=a" (a), "=d" (d));
        return (d << 32) | a;
}
#else
static UINT64 tsc() { return 0; }
#endif

static EFI_STATUS efivar_set(CHAR16 *name, CHAR16 *value, BOOLEAN persistent) {
        UINT32 flags;

        flags = EFI_VARIABLE_BOOTSERVICE_ACCESS|EFI_VARIABLE_RUNTIME_ACCESS;
        if (persistent)
                flags |= EFI_VARIABLE_NON_VOLATILE;

        return uefi_call_wrapper(RT->SetVariable, 5, name, &loader_guid, flags,
                                 value ? (StrLen(value)+1) * sizeof(CHAR16) : 0, value);
}

static EFI_STATUS efivar_get(CHAR16 *name, CHAR16 **value) {
        CHAR16 *val;
        UINTN size;
        EFI_STATUS err;

        size = sizeof(CHAR16 *) * EFI_MAXIMUM_VARIABLE_SIZE;
        val = AllocatePool(size);
        if (!val)
                return EFI_OUT_OF_RESOURCES;

        err = uefi_call_wrapper(RT->GetVariable, 5, name, &loader_guid, NULL, &size, val);
        if (EFI_ERROR(err) == 0)
                *value = val;
        else
                FreePool(val);
        return err;

}

static EFI_STATUS efivar_set_int(CHAR16 *name, INTN i, BOOLEAN persistent) {
        CHAR16 str[32];

        SPrint(str, 32, L"%d", i);
        return efivar_set(name, str, persistent);
}

static EFI_STATUS efivar_get_int(CHAR16 *name, INTN *i) {
        CHAR16 *val;
        EFI_STATUS err;

        err = efivar_get(name, &val);
        if (EFI_ERROR(err) == 0) {
                *i = Atoi(val);
                FreePool(val);
        }
        return err;
}

static VOID efivar_set_ticks(CHAR16 *name, UINT64 ticks) {
        CHAR16 str[32];

        SPrint(str, 32, L"%ld", ticks ? ticks : tsc());
        efivar_set(name, str, FALSE);
}

static BOOLEAN edit_line(CHAR16 *line_in, CHAR16 **line_out, UINTN x_max, UINTN y_pos) {
        CHAR16 *line;
        UINTN size;
        UINTN len;
        UINTN first;
        CHAR16 *print;
        UINTN cursor;
        BOOLEAN exit;
        BOOLEAN edit;

        if (!line_in)
                line_in = L"";
        size = StrLen(line_in) + 1024;
        line = AllocatePool(size * sizeof(CHAR16));
        StrCpy(line, line_in);
        len = StrLen(line);
        print = AllocatePool(x_max * sizeof(CHAR16));

        uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, TRUE);

        first = 0;
        cursor = 0;
        edit = FALSE;
        exit = FALSE;
        while (!exit) {
                UINTN index;
                EFI_STATUS err;
                EFI_INPUT_KEY key;
                UINTN i;

                i = len - first;
                if (i >= x_max-2)
                        i = x_max-2;
                CopyMem(print, line + first, i * sizeof(CHAR16));
                print[i++] = ' ';
                print[i] = '\0';

                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_pos);
                uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, print);
                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);

                uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &index);
                err = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
                if (EFI_ERROR(err))
                        continue;

                switch (key.ScanCode) {
                case SCAN_ESC:
                        exit = TRUE;
                        break;
                case SCAN_HOME:
                case SCAN_UP:
                        cursor = 0;
                        first = 0;
                        continue;
                case SCAN_END:
                case SCAN_DOWN:
                        cursor = len;
                        if (cursor >= x_max) {
                                cursor = x_max-2;
                                first = len - (x_max-2);
                        }
                        continue;
                case SCAN_RIGHT:
                        if (first + cursor == len)
                                continue;
                        if (cursor+2 < x_max)
                                cursor++;
                        else if (first + cursor < len)
                                first++;
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;
                case SCAN_LEFT:
                        if (cursor > 0)
                                cursor--;
                        else if (first > 0)
                                first--;
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;
                case SCAN_DELETE:
                        if (len == 0)
                                continue;
                        if (first + cursor == len)
                                continue;
                        for (i = first + cursor; i < len; i++)
                                line[i] = line[i+1];
                        line[len-1] = ' ';
                        len--;
                        continue;
                }

                switch (key.UnicodeChar) {
                case CHAR_LINEFEED:
                case CHAR_CARRIAGE_RETURN:
                        if (StrCmp(line, line_in) != 0) {
                                edit = TRUE;
                                *line_out = line;
                                line = NULL;
                        }
                        exit = TRUE;
                        break;
                case CHAR_BACKSPACE:
                        if (len == 0)
                                continue;
                        if (first == 0 && cursor == 0)
                                continue;
                        for (i = first + cursor-1; i < len; i++)
                                line[i] = line[i+1];
                        len--;
                        if (cursor > 0)
                                cursor--;
                        if (cursor > 0 || first == 0)
                                continue;
                        /* show full line if it fits */
                        if (len < x_max-2) {
                                cursor = first;
                                first = 0;
                                continue;
                        }
                        /* jump left to see what we delete */
                        if (first > 10) {
                                first -= 10;
                                cursor = 10;
                        } else {
                                cursor = first;
                                first = 0;
                        }
                        continue;
                case '\t':
                case ' ' ... '~':
                case 0x80 ... 0xffff:
                        if (len+1 == size)
                                continue;
                        for (i = len; i > first + cursor; i--)
                                line[i] = line[i-1];
                        line[first + cursor] = key.UnicodeChar;
                        len++;
                        line[len] = '\0';
                        if (cursor+2 < x_max)
                                cursor++;
                        else if (first + cursor < len)
                                first++;
                        continue;
                }
        }

        uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);
        FreePool(line);
        return edit;
}

static VOID menu_run(Config *config, ConfigEntry **chosen_entry) {
        EFI_STATUS err;
        INTN visible_max;
        INTN idx_highlight;
        INTN idx_highlight_prev;
        INTN idx_first;
        INTN idx_last;
        BOOLEAN refresh;
        BOOLEAN highlight;
        INTN i;
        UINTN line_width;
        CHAR16 **lines;
        UINTN x_max;
        UINTN y_max;
        CHAR16 *status;
        CHAR16 *clearline;
        INTN timeout_remain;
        BOOLEAN exit = FALSE;

        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);
        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

        err = uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, ST->ConOut->Mode->Mode, &x_max, &y_max);
        if (EFI_ERROR(err)) {
                x_max = 80;
                y_max = 25;
        }

        /* we check 10 times per second for a keystroke */
        if (config->timeout_sec > 0)
                timeout_remain = config->timeout_sec * 10;
        else
                timeout_remain = -1;

        idx_highlight = config->idx_default;
        idx_highlight_prev = 0;

        visible_max = y_max - 2;

        if (config->idx_default >= visible_max)
                idx_first = config->idx_default-1;
        else
                idx_first = 0;

        idx_last = idx_first + visible_max-1;

        refresh = TRUE;
        highlight = FALSE;

        /* length of higlighted selector bar */
        line_width = 20;
        for (i = 0; i < config->entry_count; i++) {
                UINTN entry_len;

                entry_len = StrLen(config->entries[i]->title);
                if (line_width < entry_len)
                        line_width = entry_len;
        }
        if (line_width > x_max)
                line_width = x_max;

        /* menu entries title lines */
        lines = AllocatePool(sizeof(CHAR16 *) * config->entry_count);
        for (i = 0; i < config->entry_count; i++)
                lines[i] = PoolPrint(L"  %-.*s ", line_width, config->entries[i]->title);

        status = NULL;
        clearline = AllocatePool((x_max+1) * sizeof(CHAR16));
        for (i = 0; i < x_max; i++)
                clearline[i] = ' ';
        clearline[i] = 0;

        while (!exit) {
                EFI_INPUT_KEY key;

                if (refresh) {
                        for (i = 0; i < config->entry_count; i++) {
                                if (i < idx_first || i > idx_last)
                                        continue;
                                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, i - idx_first);
                                if (i == idx_highlight)
                                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut,
                                                          EFI_BLACK|EFI_BACKGROUND_LIGHTGRAY);
                                else
                                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut,
                                                          EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                                uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, lines[i]);
                                if (i == config->idx_default_efivar) {
                                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, i - idx_first);
                                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L"*");
                                }
                        }
                        refresh = FALSE;
                } else if (highlight) {
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, idx_highlight_prev - idx_first);
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, lines[idx_highlight_prev]);
                        if (idx_highlight_prev == config->idx_default_efivar) {
                                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, idx_highlight_prev - idx_first);
                                uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L"*");
                        }

                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, idx_highlight - idx_first);
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_BLACK|EFI_BACKGROUND_LIGHTGRAY);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, lines[idx_highlight]);
                        if (idx_highlight == config->idx_default_efivar) {
                                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, idx_highlight - idx_first);
                                uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L"*");
                        }
                        highlight = FALSE;
                }

                if (timeout_remain > 0) {
                        FreePool(status);
                        status = PoolPrint(L"Boot in %d seconds.", (timeout_remain + 5) / 10);
                }

                /* print status at last line of screen */
                if (status) {
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, status);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1 + StrLen(status));
                }

                err = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
                if (err == EFI_NOT_READY) {
                        UINTN index;

                        if (timeout_remain == 0) {
                                exit = TRUE;
                                break;
                        }
                        if (timeout_remain > 0) {
                                uefi_call_wrapper(BS->Stall, 1, 100 * 1000);
                                timeout_remain--;
                                continue;
                        }
                        uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &index);
                        continue;
                }
                timeout_remain = -1;

                /* clear status after keystroke */
                if (status) {
                        FreePool(status);
                        status = NULL;
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1);
                }

                idx_highlight_prev = idx_highlight;

                switch (key.ScanCode) {
                case SCAN_UP:
                        if (idx_highlight > 0)
                                idx_highlight--;
                        break;
                case SCAN_DOWN:
                        if (idx_highlight < config->entry_count-1)
                                idx_highlight++;
                        break;
                case SCAN_HOME:
                        if (idx_highlight > 0) {
                                refresh = TRUE;
                                idx_highlight = 0;
                        }
                        break;
                case SCAN_END:
                        if (idx_highlight < config->entry_count-1) {
                                refresh = TRUE;
                                idx_highlight = config->entry_count-1;
                        }
                        break;
                case SCAN_PAGE_UP:
                        idx_highlight -= visible_max;
                        if (idx_highlight < 0)
                                idx_highlight = 0;
                        break;
                case SCAN_PAGE_DOWN:
                        idx_highlight += visible_max;
                        if (idx_highlight > config->entry_count-1)
                                idx_highlight = config->entry_count-1;
                        break;
                case SCAN_F1:
                        status = StrDuplicate(L"(d)efault, (+/-)timeout, (o)ptions, (i)nitrd, (v)ersion");
                        break;
                }

                if (idx_highlight > idx_last) {
                        idx_last = idx_highlight;
                        idx_first = 1 + idx_highlight - visible_max;
                        refresh = TRUE;
                }
                if (idx_highlight < idx_first) {
                        idx_first = idx_highlight;
                        idx_last = idx_highlight + visible_max-1;
                        refresh = TRUE;
                }
                idx_last = idx_first + visible_max-1;

                if (!refresh && idx_highlight != idx_highlight_prev)
                        highlight = TRUE;

                switch (key.UnicodeChar) {
                case CHAR_LINEFEED:
                case CHAR_CARRIAGE_RETURN:
                        exit = TRUE;
                        break;
                case 'd':
                        if (config->idx_default_efivar != idx_highlight) {
                                /* store the selected entry in a persistent EFI variable */
                                efivar_set(L"LoaderConfigDefault", config->entries[idx_highlight]->file, TRUE);
                                config->idx_default_efivar = idx_highlight;
                                status = StrDuplicate(L"Default boot entry permanently stored.");
                        } else {
                                /* clear the default entry EFI variable */
                                efivar_set(L"LoaderConfigDefault", NULL, TRUE);
                                config->idx_default_efivar = -1;
                                status = StrDuplicate(L"Default boot entry cleared.");
                        }
                        refresh = TRUE;
                        break;
                case '-':
                        if (config->timeout_sec_efivar > 0) {
                                config->timeout_sec_efivar--;
                                efivar_set_int(L"LoaderConfigTimeout", config->timeout_sec_efivar, TRUE);
                                if (config->timeout_sec_efivar > 0)
                                        status = PoolPrint(L"Menu timeout of %d sec permanently stored.",
                                                           config->timeout_sec_efivar);
                                else
                                        status = StrDuplicate(L"Menu permanently disabled. "
                                                              "Hold down key at bootup to show menu.");
                        } else if (config->timeout_sec_efivar <= 0){
                                config->timeout_sec_efivar = -1;
                                efivar_set(L"LoaderConfigTimeout", NULL, TRUE);
                                status = PoolPrint(L"Menu timeout cleared. The configured default value is %d sec.",
                                                   config->timeout_sec_config);
                        }
                        break;
                case '+':
                        config->timeout_sec_efivar++;
                        efivar_set_int(L"LoaderConfigTimeout", config->timeout_sec_efivar, TRUE);
                        if (config->timeout_sec_efivar)
                                status = PoolPrint(L"Menu timeout of %d sec permanently stored.",
                                                   config->timeout_sec_efivar);
                        else
                                status = StrDuplicate(L"Menu permanently disabled. "
                                                      "Hold down key at bootup to show menu.");
                        break;
                case 'i':
                        if (!config->entries[idx_highlight]->initrd)
                                break;
                        if (config->no_initrd) {
                                config->no_initrd = FALSE;
                                status = StrDuplicate(L"Initrd loaded at this bootup.");
                        } else {
                                config->no_initrd = TRUE;
                                status = StrDuplicate(L"Initrd not loaded at this bootup.");
                        }
                        break;
                case 'o':
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1);
                        if (edit_line(config->entries[idx_highlight]->options, &config->options_edit, x_max, y_max-1))
                                exit = TRUE;
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1);
                        break;
                case 'v':
                        status = PoolPrint(L"gummiboot %d, UEFI %d.%02d", VERSION,
                                           ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xffff);
                        break;
                }
        }

        for (i = 0; i < config->entry_count; i++)
                FreePool(lines[i]);
        FreePool(lines);
        FreePool(clearline);
        *chosen_entry = config->entries[idx_highlight];

        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_WHITE|EFI_BACKGROUND_BLACK);
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
}

static VOID config_add_entry(Config *config, ConfigEntry *entry) {
        if ((config->entry_count & 15) == 0) {
                UINTN i;

                i = config->entry_count + 16;
                if (config->entry_count == 0)
                        config->entries = AllocatePool(sizeof(VOID *) * i);
                else
                        config->entries = ReallocatePool(config->entries,
                                                         sizeof(VOID *) * config->entry_count, sizeof(VOID *) * i);
        }
        config->entries[config->entry_count++] = entry;
}

static BOOLEAN is_digit(CHAR16 c)
{
        return (c >= '0') && (c <= '9');
}

static UINTN c_order(CHAR16 c)
{
        if (c == '\0')
                return 0;
        if (is_digit(c))
                return 0;
        else if ((c >= 'a') && (c <= 'z'))
                return c;
        else
                return c + 0x10000;
}

static INTN str_verscmp(CHAR16 *s1, CHAR16 *s2)
{
        CHAR16 *os1;
        CHAR16 *os2;

        os1 = s1;
        os2 = s2;
        while (*s1 || *s2) {
                INTN first;

                while ((*s2 && !is_digit(*s1)) || (*s2 && !is_digit(*s2))) {
                        INTN order;

                        order = c_order(*s1) - c_order(*s2);
                        if (order)
                                return order;
                        s1++;
                        s2++;
                }

                while (*s1 == '0')
                        s1++;
                while (*s2 == '0')
                        s2++;

                first = 0;
                while (is_digit(*s1) && is_digit(*s2)) {
                        if (first == 0)
                                first = *s1 - *s2;
                        s1++;
                        s2++;
                }

                if (is_digit(*s1))
                        return 1;
                if (is_digit(*s2))
                        return -1;

                if (first)
                        return first;
        }

        return StrCmp(os1, os2);
}

CHAR16 *stra_to_str(CHAR8 *stra) {
        UINTN len;
        UINTN i;
        CHAR16 *str;

        len = strlena(stra);
        str = AllocatePool((len + 1) * sizeof(CHAR16));

        for (i = 0; i < len; i++)
                str[i] = stra[i];
        str[i] = '\0';

        return str;
}

CHAR16 *stra_to_path(CHAR8 *stra) {
        CHAR16 *str;
        UINTN strlen;
        UINTN len;
        UINTN i;

        len = strlena(stra);
        str = AllocatePool((len + 2) * sizeof(CHAR16));

        str[0] = '\\';
        strlen = 1;
        for (i = 0; i < len; i++) {
                if (stra[i] == '/' || stra[i] == '\\') {
                        if (str[strlen-1] == '\\')
                                continue;
                        str[strlen++] = '\\';
                        continue;
                }
                str[strlen++] = stra[i];
        }
        str[strlen] = '\0';

        return str;
}

static CHAR8 *strchra(CHAR8 *s, CHAR8 c) {
        do {
                if (*s == c)
                        return s;
        } while (*s++);
        return NULL;
}

static CHAR8 *line_get_key_value(CHAR8 *line, CHAR8 **key_ret, CHAR8 **value_ret) {
        CHAR8 *next;
        CHAR8 *key, *value;
        UINTN linelen;

        /* terminate */
skip:
        next = line;
        while (*next && !strchra((CHAR8 *)"\n\r", *next))
                next++;
        *next = '\0';

        linelen = next - line;
        if (linelen == 0)
                return NULL;

        /* next line */
        next++;
        while (*next && strchra((CHAR8 *)"\n\r", *next))
                next++;

        /* trailing whitespace */
        while (linelen && strchra((CHAR8 *)" \t", line[linelen-1]))
                linelen--;
        line[linelen] = '\0';

        /* leading whitespace */
        while (strchra((CHAR8 *)" \t", *line))
                line++;

        key = line;
        line = next;

        if (*key == '#')
                goto skip;

        /* split key/value */
        value = key;
        while (*value && !strchra((CHAR8 *)" \t", *value))
                value++;
        if (*value == '\0')
                goto skip;
        *value = '\0';
        value++;
        while (*value && strchra((CHAR8 *)" \t", *value))
                value++;

        *key_ret = key;
        *value_ret = value;
        return next;
}

static VOID config_defaults_load_from_file(Config *config, CHAR8 *content) {
        CHAR8 *line;
        CHAR8 *key, *value;

        line = content;
        while ((line = line_get_key_value(line, &key, &value))) {
                if (strcmpa((CHAR8 *)"timeout", key) == 0) {
                        CHAR16 *s;

                        s = stra_to_str(value);
                        config->timeout_sec_config = Atoi(s);
                        config->timeout_sec = config->timeout_sec_config;
                        FreePool(s);
                        continue;
                }
                if (strcmpa((CHAR8 *)"default", key) == 0) {
                        config->entry_default_pattern = stra_to_str(value);
                        StrLwr(config->entry_default_pattern);
                        continue;
                }
        }
}

static VOID config_entry_add_from_file(Config *config, CHAR16 *file, CHAR8 *content, CHAR16 *loaded_image_path) {
        ConfigEntry *entry;
        CHAR8 *line;
        CHAR8 *key, *value;
        UINTN len;

        entry = AllocateZeroPool(sizeof(ConfigEntry));

        line = content;
        while ((line = line_get_key_value(line, &key, &value))) {
                if (strcmpa((CHAR8 *)"title", key) == 0) {
                        entry->title = stra_to_str(value);
                        continue;
                }

                if (strcmpa((CHAR8 *)"linux", key) == 0) {
                        entry->type = LOADER_LINUX;
                        entry->loader = stra_to_path(value);
                        continue;
                }

                if (strcmpa((CHAR8 *)"efi", key) == 0) {
                        entry->type = LOADER_EFI;
                        entry->loader = stra_to_path(value);
                        /* do not add an entry for ourselves */
                        if (StrCmp(entry->loader, loaded_image_path) == 0) {
                                entry->type = LOADER_UNDEFINED;
                                break;
                        }
                        continue;
                }

                if (strcmpa((CHAR8 *)"initrd", key) == 0) {
                        entry->initrd = stra_to_path(value);
                        continue;
                }

                if (strcmpa((CHAR8 *)"options", key) == 0) {
                        entry->options = stra_to_str(value);
                        continue;
                }
        }

        if (entry->type == LOADER_UNDEFINED) {
                FreePool(entry->title);
                FreePool(entry->loader);
                FreePool(entry->initrd);
                FreePool(entry->options);
                FreePool(entry);
                return;
        }

        entry->file = StrDuplicate(file);
        len = StrLen(entry->file);
        /* remove ".conf" */
        if (len > 5)
                entry->file[len - 5] = '\0';
        StrLwr(entry->file);

        if (!entry->title)
                entry->title = StrDuplicate(entry->loader);

        config_add_entry(config, entry);
}

static UINTN file_read(Config *config, EFI_FILE_HANDLE dir, const CHAR16 *name, CHAR8 **content) {
        EFI_FILE_HANDLE handle;
        EFI_FILE_INFO *info;
        CHAR8 *buf;
        UINTN buflen;
        EFI_STATUS err;
        UINTN len = 0;

        err = uefi_call_wrapper(dir->Open, 5, dir, &handle, name, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(err))
                goto out;

        info = LibFileInfo(handle);
        buflen = info->FileSize+1;
        buf = AllocatePool(buflen);

        err = uefi_call_wrapper(handle->Read, 3, handle, &buflen, buf);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                buf[buflen] = '\0';
                *content = buf;
                len = buflen;
        } else
                FreePool(buf);

        FreePool(info);
        uefi_call_wrapper(handle->Close, 1, handle);
out:
        return len;
}

static VOID config_load(Config *config, EFI_FILE *root_dir, CHAR16 *loaded_image_path) {
        EFI_FILE_HANDLE entries_dir;
        EFI_STATUS err;
        CHAR8 *content;
        INTN sec;
        UINTN len;
        UINTN i;

        len = file_read(config, root_dir, L"\\loader\\loader.conf", &content);
        if (len > 0)
                config_defaults_load_from_file(config, content);

        err = efivar_get_int(L"LoaderConfigTimeout", &sec);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                config->timeout_sec_efivar = sec;
                config->timeout_sec = sec;
        } else
                config->timeout_sec_efivar = -1;

        err = uefi_call_wrapper(root_dir->Open, 5, root_dir, &entries_dir, L"\\loader\\entries", EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                for (;;) {
                        CHAR16 buf[256];
                        UINTN bufsize;
                        EFI_FILE_INFO *f;
                        CHAR8 *content;
                        UINTN len;

                        bufsize = sizeof(buf);
                        err = uefi_call_wrapper(entries_dir->Read, 3, entries_dir, &bufsize, buf);
                        if (bufsize == 0 || EFI_ERROR(err))
                                break;

                        f = (EFI_FILE_INFO *) buf;
                        if (f->FileName[0] == '.')
                                continue;
                        if (f->Attribute & EFI_FILE_DIRECTORY)
                                continue;
                        len = StrLen(f->FileName);
                        if (len < 6)
                                continue;
                        if (StriCmp(f->FileName + len - 5, L".conf") != 0)
                                continue;

                        len = file_read(config, entries_dir, f->FileName, &content);
                        if (len > 0)
                                config_entry_add_from_file(config, f->FileName, content, loaded_image_path);
                }
                uefi_call_wrapper(entries_dir->Close, 1, entries_dir);
        }

        /* sort entries after version number */
        for (i = 1; i < config->entry_count; i++) {
                BOOLEAN more;
                UINTN j;

                more = FALSE;
                for (j = 0; j < config->entry_count - i; j++) {
                        ConfigEntry *entry;

                        if (str_verscmp(config->entries[j]->file, config->entries[j+1]->file) <= 0)
                                continue;
                        entry = config->entries[j];
                        config->entries[j] = config->entries[j+1];
                        config->entries[j+1] = entry;
                        more = TRUE;
                }
                if (!more)
                        break;
        }
}

static VOID config_default_entry_select(Config *config) {
        CHAR16 *var;
        EFI_STATUS err;

        /*
         * The EFI variable to specify a boot entry for the next, and only the
         * next reboot. The variable is always cleared directly after it is read.
         */
        err = efivar_get(L"LoaderEntryOneShot", &var);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                BOOLEAN found = FALSE;
                UINTN i;

                for (i = 0; i < config->entry_count; i++) {
                        if (!config->entries[i]->file)
                                continue;
                        if (StrCmp(config->entries[i]->file, var) == 0) {
                                config->idx_default = i;
                                found = TRUE;
                                break;
                        }
                }
                efivar_set(L"LoaderEntryOneShot", NULL, TRUE);
                FreePool(var);
                if (found)
                        return;
        }

        /*
         * The EFI variable to select the default boot entry overrides the
         * configured pattern. The variable can be set and cleared by pressing
         * the 'd' key in the loader selection menu, the entry is marked with
         * an '*'.
         */
        err = efivar_get(L"LoaderConfigDefault", &var);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                BOOLEAN found = FALSE;
                UINTN i;

                for (i = 0; i < config->entry_count; i++) {
                        if (!config->entries[i]->file)
                                continue;
                        if (StrCmp(config->entries[i]->file, var) == 0) {
                                config->idx_default = i;
                                config->idx_default_efivar = i;
                                found = TRUE;
                                break;
                        }
                }
                FreePool(var);
                if (found)
                        return;
        }
        config->idx_default_efivar = -1;

        /*
         * Match the pattern from the end of the list to the start, find last
         * entry (largest number) matching the given pattern.
         */
        if (config->entry_default_pattern) {
                UINTN i;

                for (i = config->entry_count-1; i >= 0; i--) {
                        if (!config->entries[i]->file)
                                continue;
                        if (config->entries[i]->no_default)
                                continue;
                        if (MetaiMatch(config->entries[i]->file, config->entry_default_pattern)) {
                                config->idx_default = i;
                                return;
                        }
                }
        }

        /* select the last entry */
        if (config->entry_count)
                config->idx_default = config->entry_count-1;
}

static VOID config_entry_add_loader(Config *config, EFI_FILE *root_dir, CHAR16 *file, CHAR16 *title, CHAR16 *loader) {
        EFI_FILE_HANDLE handle;
        EFI_STATUS err;
        ConfigEntry *entry;

        /* check existence */
        err = uefi_call_wrapper(root_dir->Open, 5, root_dir, &handle, loader, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(err))
                return;
        uefi_call_wrapper(handle->Close, 1, handle);

        entry = AllocateZeroPool(sizeof(ConfigEntry));
        entry->title = StrDuplicate(title);
        entry->loader = StrDuplicate(loader);
        if (file)
                entry->file = StrDuplicate(file);
        entry->no_default = TRUE;
        config_add_entry(config, entry);
}

static EFI_STATUS image_start(EFI_HANDLE parent_image, EFI_LOADED_IMAGE *parent_loaded_image,
                              const Config *config, const ConfigEntry *entry) {
        EFI_STATUS err;
        EFI_HANDLE image;
        EFI_DEVICE_PATH *path;
        EFI_LOADED_IMAGE *loaded_image;
        CHAR16 *options;

        path = FileDevicePath(parent_loaded_image->DeviceHandle, entry->loader);
        if (!path) {
                Print(L"Error getting device path.");
                uefi_call_wrapper(BS->Stall, 1, 1000 * 1000);
                return EFI_INVALID_PARAMETER;
        }

        err = uefi_call_wrapper(BS->LoadImage, 6, FALSE, parent_image, path, NULL, 0, &image);
        if (EFI_ERROR(err)) {
                Print(L"Error loading %s: %r", entry->loader, err);
                uefi_call_wrapper(BS->Stall, 1, 1000 * 1000);
                goto out;
        }

        if (config->options_edit)
                options = config->options_edit;
        else if (entry->options)
                options = entry->options;
        else
                options = NULL;
        if (options || (entry->initrd && !config->no_initrd)) {
                err = uefi_call_wrapper(BS->OpenProtocol, 6, image, &LoadedImageProtocol, &loaded_image,
                                        parent_image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
                if (EFI_ERROR(err)) {
                        Print(L"Error getting LoadedImageProtocol handle: %r", err);
                        uefi_call_wrapper(BS->Stall, 1, 1000 * 1000);
                        goto out_unload;
                }
                if (entry->type == LOADER_LINUX && entry->initrd && !config->no_initrd) {
                        if (options)
                                loaded_image->LoadOptions = PoolPrint(L"initrd=%s %s", entry->initrd, options);
                        else
                                loaded_image->LoadOptions = PoolPrint(L"initrd=%s", entry->initrd);
                } else
                        loaded_image->LoadOptions = options;
                loaded_image->LoadOptionsSize = (StrLen(loaded_image->LoadOptions)+1) * sizeof(CHAR16);
        }

        efivar_set_ticks(L"LoaderTicksStartImage", 0);
        err = uefi_call_wrapper(BS->StartImage, 3, image, NULL, NULL);
out_unload:
        uefi_call_wrapper(BS->UnloadImage, 1, image);
out:
        FreePool(path);
        return err;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table) {
        EFI_LOADED_IMAGE *loaded_image;
        EFI_FILE *root_dir;
        CHAR16 *loaded_image_path;
        EFI_STATUS err;
        Config config;
        UINT64 ticks;
        BOOLEAN menu = FALSE;

        ticks = tsc();
        InitializeLib(image, sys_table);
        efivar_set_ticks(L"LoaderTicksInit", ticks);

        err = uefi_call_wrapper(BS->OpenProtocol, 6, image, &LoadedImageProtocol, &loaded_image,
                                image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(err)) {
                Print(L"Error getting a LoadedImageProtocol handle: %r ", err);
                uefi_call_wrapper(BS->Stall, 1, 1000 * 1000);
                return err;
        }

        root_dir = LibOpenRoot(loaded_image->DeviceHandle);
        if (!root_dir) {
                Print(L"Unable to open root directory: %r ", err);
                uefi_call_wrapper(BS->Stall, 1, 1000 * 1000);
                return EFI_LOAD_ERROR;
        }

        ZeroMem(&config, sizeof(Config));

        /* scan "\loader\entries\*.conf" files */
        loaded_image_path = DevicePathToStr(loaded_image->FilePath);
        config_load(&config, root_dir, loaded_image_path);
        FreePool(loaded_image_path);

        /* add fallback entry to the end of the list */
        config_entry_add_loader(&config, root_dir, L"fallback", L"EFI default loader", L"\\EFI\\BOOT\\BOOTX64.EFI");

        /* select entry by configured pattern or EFI LoaderDefaultEntry= variable*/
        config_default_entry_select(&config);

        /* show menu when key is pressed or timeout is set */
        if (config.timeout_sec == 0) {
                EFI_INPUT_KEY key;

                err = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
                menu = err != EFI_NOT_READY;
        } else
                menu = TRUE;

        for (;;) {
                ConfigEntry *entry;

                entry = config.entries[config.idx_default];
                if (menu) {
                        efivar_set_ticks(L"LoaderTicksStartMenu", 0);
                        menu_run(&config, &entry);
                }

                /* export the selected boot entry to the system */
                err = efivar_set(L"LoaderEntrySelected",  entry->file, FALSE);
                if (EFI_ERROR(err)) {
                       Print(L"Error storing LoaderEntrySelected variable: %r ", err);
                       uefi_call_wrapper(BS->Stall, 1, 1000 * 1000);
                }

                image_start(image, loaded_image, &config, entry);

                menu = TRUE;
                config.timeout_sec = 0;
        }

        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        uefi_call_wrapper(BS->CloseProtocol, 4, image, &LoadedImageProtocol, image, NULL);
        return EFI_SUCCESS;
}
