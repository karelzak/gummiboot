/*
 * Simple UEFI boot loader which executes configured EFI images, where the
 * default entry is selected by a configured pattern (glob) or an on-screen
 * menu.
 *
 * All gummiboot code is LGPL not GPL, to stay out of politics and to give
 * the freedom of copying code from programs to possible future libraries.
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

#define _stringify(s) #s
#define stringify(s) _stringify(s)

#ifndef EFI_SECURITY_VIOLATION
#define EFI_SECURITY_VIOLATION      EFIERR(26)
#endif

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
        CHAR16 *title_show;
        CHAR16 *title;
        CHAR16 *version;
        CHAR16 *machine_id;
        EFI_HANDLE *device;
        enum loader_type type;
        CHAR16 *loader;
        CHAR16 *options;
        BOOLEAN no_autoselect;
        BOOLEAN non_unique;
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
        CHAR16 *entries_auto;
} Config;

#ifdef __x86_64__
static UINT64 ticks_read() {
        UINT64 a, d;
        __asm__ volatile ("rdtsc" : "=a" (a), "=d" (d));
        return (d << 32) | a;
}
#else
static UINT64 ticks_read() { return 0; }
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
        if (EFI_ERROR(err) == EFI_SUCCESS)
                *value = val;
        else
                FreePool(val);
        return err;

}

static EFI_STATUS efivar_set_int(CHAR16 *name, UINTN i, BOOLEAN persistent) {
        CHAR16 str[32];

        SPrint(str, 32, L"%d", i);
        return efivar_set(name, str, persistent);
}

static EFI_STATUS efivar_get_int(CHAR16 *name, UINTN *i) {
        CHAR16 *val;
        EFI_STATUS err;

        err = efivar_get(name, &val);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                *i = Atoi(val);
                FreePool(val);
        }
        return err;
}

static VOID efivar_set_ticks(CHAR16 *name, UINT64 ticks) {
        CHAR16 str[32];

        if (ticks == 0)
                ticks = ticks_read();
        if (ticks == 0)
                return;

        SPrint(str, 32, L"%ld", ticks ? ticks : ticks_read());
        efivar_set(name, str, FALSE);
}

static void cursor_left(UINTN *cursor, UINTN *first)
{
        if ((*cursor) > 0)
                (*cursor)--;
        else if ((*first) > 0)
                (*first)--;
}

static void cursor_right(UINTN *cursor, UINTN *first, UINTN x_max, UINTN len)
{
        if ((*cursor)+2 < x_max)
                (*cursor)++;
        else if ((*first) + (*cursor) < len)
                (*first)++;
}

static BOOLEAN line_edit(CHAR16 *line_in, CHAR16 **line_out, UINTN x_max, UINTN y_pos) {
        CHAR16 *line;
        UINTN size;
        UINTN len;
        UINTN first;
        CHAR16 *print;
        UINTN cursor;
        BOOLEAN exit;
        BOOLEAN enter;

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
        enter = FALSE;
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
                        cursor = 0;
                        first = 0;
                        continue;
                case SCAN_END:
                        cursor = len;
                        if (cursor >= x_max) {
                                cursor = x_max-2;
                                first = len - (x_max-2);
                        }
                        continue;
                case SCAN_UP:
                        while((first + cursor) && line[first + cursor] == ' ')
                                cursor_left(&cursor, &first);
                        while((first + cursor) && line[first + cursor] != ' ')
                                cursor_left(&cursor, &first);
                        while((first + cursor) && line[first + cursor] == ' ')
                                cursor_left(&cursor, &first);
                        if (first + cursor != len && first + cursor)
                                cursor_right(&cursor, &first, x_max, len);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;
                case SCAN_DOWN:
                        while(line[first + cursor] && line[first + cursor] == ' ')
                                cursor_right(&cursor, &first, x_max, len);
                        while(line[first + cursor] && line[first + cursor] != ' ')
                                cursor_right(&cursor, &first, x_max, len);
                        while(line[first + cursor] && line[first + cursor] == ' ')
                                cursor_right(&cursor, &first, x_max, len);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;
                case SCAN_RIGHT:
                        if (first + cursor == len)
                                continue;
                        cursor_right(&cursor, &first, x_max, len);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, cursor, y_pos);
                        continue;
                case SCAN_LEFT:
                        cursor_left(&cursor, &first);
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
                                *line_out = line;
                                line = NULL;
                        }
                        enter = TRUE;
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
        FreePool(print);
        FreePool(line);
        return enter;
}

static VOID dump_status(Config *config, CHAR16 *loaded_image_path) {
        UINTN index;
        UINTN i;
        CHAR16 *s;

        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

        Print(L"gummiboot version:      " stringify(VERSION) "\n");
        Print(L"loaded image:           %s\n", loaded_image_path);
        Print(L"UEFI version:           %d.%02d\n", ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xffff);
        Print(L"firmware vendor:        %s\n", ST->FirmwareVendor);
        Print(L"firmware version:       %d.%02d\n", ST->FirmwareRevision >> 16, ST->FirmwareRevision & 0xffff);
        Print(L"\n");

        Print(L"timeout:                %d\n", config->timeout_sec);
        if (config->timeout_sec_efivar >= 0)
                Print(L"timeout (EFI var):      %d\n", config->timeout_sec_efivar);
        Print(L"timeout (config):       %d\n", config->timeout_sec_config);
        Print(L"default pattern:        '%s'\n", config->entry_default_pattern);
        Print(L"\n");

        Print(L"config entry count:     %d\n", config->entry_count);
        Print(L"entry selected idx:     %d\n", config->idx_default);
        if (config->idx_default_efivar >= 0)
                Print(L"entry EFI var idx:      %d\n", config->idx_default_efivar);
        Print(L"\n");

        if (efivar_get_int(L"LoaderConfigTimeout", &i) == EFI_SUCCESS)
                Print(L"LoaderConfigTimeout:    %d\n", i);
        if (efivar_get(L"LoaderEntryOneShot", &s) == EFI_SUCCESS) {
                Print(L"LoaderEntryOneShot:     %s\n", s);
                FreePool(s);
        }
        if (efivar_get(L"LoaderDeviceIdentifier", &s) == EFI_SUCCESS) {
                Print(L"LoaderDeviceIdentifier: %s\n", s);
                FreePool(s);
        }
        if (efivar_get(L"LoaderDevicePartUUID", &s) == EFI_SUCCESS) {
                Print(L"LoaderDevicePartUUID:   %s\n", s);
                FreePool(s);
        }
        if (efivar_get(L"LoaderEntryDefault", &s) == EFI_SUCCESS) {
                Print(L"LoaderEntryDefault:     %s\n", s);
                FreePool(s);
        }

        Print(L"\n--- press key ---\n\n");
        uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &index);
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);

        for (i = 0; i < config->entry_count; i++) {
                ConfigEntry *entry;
                EFI_DEVICE_PATH *device_path;
                CHAR16 *str;

                entry = config->entries[i];
                Print(L"config entry:           %d/%d\n", i+1, config->entry_count);
                Print(L"file                    '%s'\n", entry->file);
                Print(L"title show              '%s'\n", entry->title_show);
                if (entry->title)
                        Print(L"title                   '%s'\n", entry->title);
                if (entry->version)
                        Print(L"version                 '%s'\n", entry->version);
                if (entry->machine_id)
                        Print(L"machine-id              '%s'\n", entry->machine_id);
                device_path = DevicePathFromHandle(entry->device);
                if (device_path) {
                        str = DevicePathToStr(device_path);
                        Print(L"device handle           '%s'\n", DevicePathToStr(device_path));
                        FreePool(str);
                }
                Print(L"loader                  '%s'\n", entry->loader);
                if (entry->options)
                        Print(L"options                 '%s'\n", entry->options);
                Print(L"auto-select             %s\n", entry->no_autoselect ? L"no" : L"yes");
                Print(L"\n--- press key ---\n\n");
                uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &index);
                uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        }

        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
}

static EFI_STATUS console_text_mode(VOID) {
        #define EFI_CONSOLE_CONTROL_PROTOCOL_GUID \
                { 0xf42f7782, 0x12e, 0x4c12, { 0x99, 0x56, 0x49, 0xf9, 0x43, 0x4, 0xf7, 0x21 }};

        struct _EFI_CONSOLE_CONTROL_PROTOCOL;

        typedef enum {
                EfiConsoleControlScreenText,
                EfiConsoleControlScreenGraphics,
                EfiConsoleControlScreenMaxValue,
        } EFI_CONSOLE_CONTROL_SCREEN_MODE;

        typedef EFI_STATUS (*EFI_CONSOLE_CONTROL_PROTOCOL_GET_MODE)(
                struct _EFI_CONSOLE_CONTROL_PROTOCOL *This,
                EFI_CONSOLE_CONTROL_SCREEN_MODE *Mode,
                BOOLEAN *UgaExists,
                BOOLEAN *StdInLocked
        );

        typedef EFI_STATUS (*EFI_CONSOLE_CONTROL_PROTOCOL_SET_MODE)(
                struct _EFI_CONSOLE_CONTROL_PROTOCOL *This,
                EFI_CONSOLE_CONTROL_SCREEN_MODE Mode
        );

        typedef EFI_STATUS (*EFI_CONSOLE_CONTROL_PROTOCOL_LOCK_STD_IN)(
                struct _EFI_CONSOLE_CONTROL_PROTOCOL *This,
                CHAR16 *Password
        );

        typedef struct _EFI_CONSOLE_CONTROL_PROTOCOL {
                EFI_CONSOLE_CONTROL_PROTOCOL_GET_MODE GetMode;
                EFI_CONSOLE_CONTROL_PROTOCOL_SET_MODE SetMode;
                EFI_CONSOLE_CONTROL_PROTOCOL_LOCK_STD_IN LockStdIn;
        } EFI_CONSOLE_CONTROL_PROTOCOL;

        EFI_GUID ConsoleControlProtocolGuid = EFI_CONSOLE_CONTROL_PROTOCOL_GUID;
        EFI_CONSOLE_CONTROL_PROTOCOL *ConsoleControl = NULL;
        EFI_STATUS err;

        err = LibLocateProtocol(&ConsoleControlProtocolGuid, (VOID **)&ConsoleControl);
        if (EFI_ERROR(err))
                return err;
        return uefi_call_wrapper(ConsoleControl->SetMode, 2, ConsoleControl, EfiConsoleControlScreenText);
}

static BOOLEAN menu_run(Config *config, ConfigEntry **chosen_entry, CHAR16 *loaded_image_path) {
        EFI_STATUS err;
        UINTN visible_max;
        UINTN idx_highlight;
        UINTN idx_highlight_prev;
        UINTN idx_first;
        UINTN idx_last;
        BOOLEAN refresh;
        BOOLEAN highlight;
        UINTN i;
        UINTN line_width;
        CHAR16 **lines;
        UINTN x_max;
        UINTN y_max;
        CHAR16 *status;
        CHAR16 *clearline;
        INTN timeout_remain;
        BOOLEAN exit = FALSE;
        BOOLEAN run = TRUE;

        console_text_mode();
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);
        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

        err = uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, ST->ConOut->Mode->Mode, &x_max, &y_max);
        if (EFI_ERROR(err)) {
                x_max = 80;
                y_max = 25;
        }
        /* reserve some space at the beginning of the line and for the cursor at the end */
        x_max-=3;

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

                entry_len = StrLen(config->entries[i]->title_show);
                if (line_width < entry_len)
                        line_width = entry_len;
        }
        if (line_width > x_max)
                line_width = x_max;

        /* menu entries title lines */
        lines = AllocatePool(sizeof(CHAR16 *) * config->entry_count);
        for (i = 0; i < config->entry_count; i++)
                lines[i] = PoolPrint(L"  %-.*s  ", line_width, config->entries[i]->title_show);

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
                                if ((INTN)i == config->idx_default_efivar) {
                                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, i - idx_first);
                                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L"*");
                                }
                        }
                        refresh = FALSE;
                } else if (highlight) {
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, idx_highlight_prev - idx_first);
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, lines[idx_highlight_prev]);
                        if ((INTN)idx_highlight_prev == config->idx_default_efivar) {
                                uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, idx_highlight_prev - idx_first);
                                uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L"*");
                        }

                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, idx_highlight - idx_first);
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_BLACK|EFI_BACKGROUND_LIGHTGRAY);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, lines[idx_highlight]);
                        if ((INTN)idx_highlight == config->idx_default_efivar) {
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
                        status = StrDuplicate(L"(d)efault, (+/-)timeout, (e)dit, (v)ersion (q)uit (*)dump");
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
                case 'q':
                        exit = TRUE;
                        run = FALSE;
                        break;
                case 'd':
                        if (config->idx_default_efivar != (INTN)idx_highlight) {
                                /* store the selected entry in a persistent EFI variable */
                                efivar_set(L"LoaderEntryDefault", config->entries[idx_highlight]->file, TRUE);
                                config->idx_default_efivar = idx_highlight;
                                status = StrDuplicate(L"Default boot entry permanently stored.");
                        } else {
                                /* clear the default entry EFI variable */
                                efivar_set(L"LoaderEntryDefault", NULL, TRUE);
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
                                if (config->timeout_sec_config > 0)
                                        status = PoolPrint(L"Menu timeout of %d sec defined by configuration file.",
                                                           config->timeout_sec_config);
                                else
                                        status = StrDuplicate(L"Menu permanently disabled. "
                                                              "Hold down key at bootup to show menu.");
                        }
                        break;
                case '+':
                        if (config->timeout_sec_efivar == -1 && config->timeout_sec_config == 0)
                                config->timeout_sec_efivar++;
                        config->timeout_sec_efivar++;
                        efivar_set_int(L"LoaderConfigTimeout", config->timeout_sec_efivar, TRUE);
                        if (config->timeout_sec_efivar > 0)
                                status = PoolPrint(L"Menu timeout of %d sec permanently stored.",
                                                   config->timeout_sec_efivar);
                        else
                                status = StrDuplicate(L"Menu permanently disabled. "
                                                      "Hold down key at bootup to show menu.");
                        break;
                case 'e':
                        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_LIGHTGRAY|EFI_BACKGROUND_BLACK);
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1);
                        if (line_edit(config->entries[idx_highlight]->options, &config->options_edit, x_max, y_max-1))
                                exit = TRUE;
                        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, y_max-1);
                        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, clearline+1);
                        break;
                case 'v':
                        status = PoolPrint(L"gummiboot " stringify(VERSION) ", UEFI %d.%02d, %s %d.%02d",
                                           ST->Hdr.Revision >> 16, ST->Hdr.Revision & 0xffff,
                                           ST->FirmwareVendor, ST->FirmwareRevision >> 16, ST->FirmwareRevision & 0xffff);
                        break;
                case '*':
                        dump_status(config, loaded_image_path);
                        refresh = TRUE;
                        break;
                }
        }

        *chosen_entry = config->entries[idx_highlight];

        for (i = 0; i < config->entry_count; i++)
                FreePool(lines[i]);
        FreePool(lines);
        FreePool(clearline);

        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, EFI_WHITE|EFI_BACKGROUND_BLACK);
        uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
        return run;
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

static VOID config_entry_free(ConfigEntry *entry) {
        FreePool(entry->title_show);
        FreePool(entry->title);
        FreePool(entry->machine_id);
        FreePool(entry->loader);
        FreePool(entry->options);
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
        CHAR16 *os1 = s1;
        CHAR16 *os2 = s2;

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

static INTN utf8_to_16(CHAR8 *stra, CHAR16 *c) {
        CHAR16 unichar;
        UINTN len;
        UINTN i;

        if (stra[0] < 0x80)
                len = 1;
        else if ((stra[0] & 0xe0) == 0xc0)
                len = 2;
        else if ((stra[0] & 0xf0) == 0xe0)
                len = 3;
        else if ((stra[0] & 0xf8) == 0xf0)
                len = 4;
        else if ((stra[0] & 0xfc) == 0xf8)
                len = 5;
        else if ((stra[0] & 0xfe) == 0xfc)
                len = 6;
        else
                return -1;

        switch (len) {
        case 1:
                unichar = stra[0];
                break;
        case 2:
                unichar = stra[0] & 0x1f;
                break;
        case 3:
                unichar = stra[0] & 0x0f;
                break;
        case 4:
                unichar = stra[0] & 0x07;
                break;
        case 5:
                unichar = stra[0] & 0x03;
                break;
        case 6:
                unichar = stra[0] & 0x01;
                break;
        }

        for (i = 1; i < len; i++) {
                if ((stra[i] & 0xc0) != 0x80)
                        return -1;
                unichar <<= 6;
                unichar |= stra[i] & 0x3f;
        }

        *c = unichar;
        return len;
}

CHAR16 *stra_to_str(CHAR8 *stra) {
        UINTN strlen;
        UINTN len;
        UINTN i;
        CHAR16 *str;

        len = strlena(stra);
        str = AllocatePool((len + 1) * sizeof(CHAR16));

        strlen = 0;
        i = 0;
        while (i < len) {
                INTN utf8len;

                utf8len = utf8_to_16(stra + i, str + strlen);
                if (utf8len <= 0) {
                        /* invalid utf8 sequence, skip the garbage */
                        i++;
                        continue;
                }

                strlen++;
                i += utf8len;
        }
        str[strlen] = '\0';
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
        i = 0;
        while (i < len) {
                INTN utf8len;

                utf8len = utf8_to_16(stra + i, str + strlen);
                if (utf8len <= 0) {
                        /* invalid utf8 sequence, skip the garbage */
                        i++;
                        continue;
                }

                if (str[strlen] == '/')
                        str[strlen] = '\\';
                if (str[strlen] == '\\' && str[strlen-1] == '\\') {
                        /* skip double slashes */
                        i += utf8len;
                        continue;
                }

                strlen++;
                i += utf8len;
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

static CHAR8 *line_get_key_value(CHAR8 *content, UINTN *pos, CHAR8 **key_ret, CHAR8 **value_ret) {
        CHAR8 *line;
        UINTN linelen;
        CHAR8 *value;

skip:
        line = content + *pos;
        if (*line == '\0')
                return NULL;

        linelen = 0;
        while (line[linelen] && !strchra((CHAR8 *)"\n\r", line[linelen]))
               linelen++;

        /* move pos to next line */
        *pos += linelen;
        if (content[*pos])
                (*pos)++;

        /* empty line */
        if (linelen == 0)
                goto skip;

        /* terminate line */
        line[linelen] = '\0';

        /* remove leading whitespace */
        while (strchra((CHAR8 *)" \t", *line)) {
                line++;
                linelen--;
        }

        /* remove trailing whitespace */
        while (linelen > 0 && strchra((CHAR8 *)" \t", line[linelen-1]))
                linelen--;
        line[linelen] = '\0';

        if (*line == '#')
                goto skip;

        /* split key/value */
        value = line;
        while (*value && !strchra((CHAR8 *)" \t", *value))
                value++;
        if (*value == '\0')
                goto skip;
        *value = '\0';
        value++;
        while (*value && strchra((CHAR8 *)" \t", *value))
                value++;

        *key_ret = line;
        *value_ret = value;
        return line;
}

static VOID config_defaults_load_from_file(Config *config, CHAR8 *content) {
        CHAR8 *line;
        UINTN pos = 0;
        CHAR8 *key, *value;

        line = content;
        while ((line = line_get_key_value(content, &pos, &key, &value))) {
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

static VOID config_entry_add_from_file(Config *config, EFI_HANDLE *device, CHAR16 *file, CHAR8 *content, CHAR16 *loaded_image_path) {
        ConfigEntry *entry;
        CHAR8 *line;
        UINTN pos = 0;
        CHAR8 *key, *value;
        UINTN len;
        CHAR16 *initrd = NULL;

        entry = AllocateZeroPool(sizeof(ConfigEntry));

        line = content;
        while ((line = line_get_key_value(content, &pos, &key, &value))) {
                if (strcmpa((CHAR8 *)"title", key) == 0) {
                        FreePool(entry->title);
                        entry->title = stra_to_str(value);
                        continue;
                }

                if (strcmpa((CHAR8 *)"version", key) == 0) {
                        FreePool(entry->version);
                        entry->version = stra_to_str(value);
                        continue;
                }

                if (strcmpa((CHAR8 *)"machine-id", key) == 0) {
                        FreePool(entry->machine_id);
                        entry->machine_id = stra_to_str(value);
                        continue;
                }

                if (strcmpa((CHAR8 *)"linux", key) == 0) {
                        FreePool(entry->loader);
                        entry->type = LOADER_LINUX;
                        entry->loader = stra_to_path(value);
                        continue;
                }

                if (strcmpa((CHAR8 *)"efi", key) == 0) {
                        entry->type = LOADER_EFI;
                        FreePool(entry->loader);
                        entry->loader = stra_to_path(value);

                        /* do not add an entry for ourselves */
                        if (StriCmp(entry->loader, loaded_image_path) == 0) {
                                entry->type = LOADER_UNDEFINED;
                                break;
                        }
                        continue;
                }

                if (strcmpa((CHAR8 *)"initrd", key) == 0) {
                        CHAR16 *new;

                        new = stra_to_path(value);
                        if (initrd) {
                                CHAR16 *s;

                                s = PoolPrint(L"%s initrd=%s", initrd, new);
                                FreePool(initrd);
                                initrd = s;
                        } else
                                initrd = PoolPrint(L"initrd=%s", new);
                        FreePool(new);
                        continue;
                }

                if (strcmpa((CHAR8 *)"options", key) == 0) {
                        CHAR16 *new;

                        new = stra_to_str(value);
                        if (entry->options) {
                                CHAR16 *s;

                                s = PoolPrint(L"%s %s", entry->options, new);
                                FreePool(entry->options);
                                entry->options = s;
                        } else {
                                entry->options = new;
                                new = NULL;
                        }
                        FreePool(new);
                        continue;
                }
        }

        if (entry->type == LOADER_UNDEFINED) {
                config_entry_free(entry);
                FreePool(initrd);
                FreePool(entry);
                return;
        }

        /* add initrd= to options */
        if (entry->type == LOADER_LINUX && initrd) {
                if (entry->options) {
                        CHAR16 *s;

                        s = PoolPrint(L"%s %s", initrd, entry->options);
                        FreePool(entry->options);
                        entry->options = s;
                } else {
                        entry->options = initrd;
                        initrd = NULL;
                }
        }
        FreePool(initrd);

        if (entry->machine_id) {
                CHAR16 *var;

                /* append additional options from EFI variables for this machine-id */
                var = PoolPrint(L"LoaderEntryOptions-%s", entry->machine_id);
                if (var) {
                        CHAR16 *s;

                        if (efivar_get(var, &s) == EFI_SUCCESS) {
                                if (entry->options) {
                                        CHAR16 *s2;

                                        s2 = PoolPrint(L"%s %s", entry->options, s);
                                        FreePool(entry->options);
                                        entry->options = s2;
                                } else
                                        entry->options = s;
                        }
                        FreePool(var);
                }

                var = PoolPrint(L"LoaderEntryOptionsOneShot-%s", entry->machine_id);
                if (var) {
                        CHAR16 *s;

                        if (efivar_get(var, &s) == EFI_SUCCESS) {
                                if (entry->options) {
                                        CHAR16 *s2;

                                        s2 = PoolPrint(L"%s %s", entry->options, s);
                                        FreePool(entry->options);
                                        entry->options = s2;
                                } else
                                        entry->options = s;
                                efivar_set(var, NULL, TRUE);
                        }
                        FreePool(var);
                }
        }

        entry->device = device;
        entry->file = StrDuplicate(file);
        len = StrLen(entry->file);
        /* remove ".conf" */
        if (len > 5)
                entry->file[len - 5] = '\0';
        StrLwr(entry->file);

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

static VOID config_load(Config *config, EFI_HANDLE *device, EFI_FILE *root_dir, CHAR16 *loaded_image_path) {
        EFI_FILE_HANDLE entries_dir;
        EFI_STATUS err;
        CHAR8 *content = NULL;
        UINTN sec;
        UINTN len;
        UINTN i;

        len = file_read(config, root_dir, L"\\loader\\loader.conf", &content);
        if (len > 0)
                config_defaults_load_from_file(config, content);
        FreePool(content);

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
                        CHAR8 *content = NULL;
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
                                config_entry_add_from_file(config, device, f->FileName, content, loaded_image_path);
                        FreePool(content);
                }
                uefi_call_wrapper(entries_dir->Close, 1, entries_dir);
        }

        /* sort entries after version number */
        for (i = 1; i < config->entry_count; i++) {
                BOOLEAN more;
                UINTN k;

                more = FALSE;
                for (k = 0; k < config->entry_count - i; k++) {
                        ConfigEntry *entry;

                        if (str_verscmp(config->entries[k]->file, config->entries[k+1]->file) <= 0)
                                continue;
                        entry = config->entries[k];
                        config->entries[k] = config->entries[k+1];
                        config->entries[k+1] = entry;
                        more = TRUE;
                }
                if (!more)
                        break;
        }
}

static VOID config_default_entry_select(Config *config) {
        CHAR16 *var;
        EFI_STATUS err;
        UINTN i;

        /*
         * The EFI variable to specify a boot entry for the next, and only the
         * next reboot. The variable is always cleared directly after it is read.
         */
        err = efivar_get(L"LoaderEntryOneShot", &var);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                BOOLEAN found = FALSE;

                for (i = 0; i < config->entry_count; i++) {
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
        err = efivar_get(L"LoaderEntryDefault", &var);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                BOOLEAN found = FALSE;

                for (i = 0; i < config->entry_count; i++) {
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

        if (config->entry_count == 0)
                return;

        /*
         * Match the pattern from the end of the list to the start, find last
         * entry (largest number) matching the given pattern.
         */
        if (config->entry_default_pattern) {
                i = config->entry_count;
                while (i--) {
                        if (config->entries[i]->no_autoselect)
                                continue;
                        if (MetaiMatch(config->entries[i]->file, config->entry_default_pattern)) {
                                config->idx_default = i;
                                return;
                        }
                }
        }

        /* select the last entry */
        i = config->entry_count;
        while (i--) {
                if (config->entries[i]->no_autoselect)
                        continue;
                config->idx_default = i;
                return;
        }

        config->idx_default = config->entry_count-1;
}

/* generate a unique title, avoiding non-distinguishable menu entries */
static VOID config_title_generate(Config *config) {
        UINTN i, k;
        BOOLEAN unique;

        /* set title */
        for (i = 0; i < config->entry_count; i++) {
                CHAR16 *title;

                FreePool(config->entries[i]->title_show);
                title = config->entries[i]->title;
                if (!title)
                        title = config->entries[i]->file;
                config->entries[i]->title_show = StrDuplicate(title);
        }

        unique = TRUE;
        for (i = 0; i < config->entry_count; i++) {
                for (k = 0; k < config->entry_count; k++) {
                        if (i == k)
                                continue;
                        if (StrCmp(config->entries[i]->title_show, config->entries[k]->title_show) != 0)
                                continue;

                        unique = FALSE;
                        config->entries[i]->non_unique = TRUE;
                        config->entries[k]->non_unique = TRUE;
                }
        }
        if (unique)
                return;

        /* add version to non-unique titles */
        for (i = 0; i < config->entry_count; i++) {
                CHAR16 *s;

                if (!config->entries[i]->non_unique)
                        continue;
                if (!config->entries[i]->version)
                        continue;

                s = PoolPrint(L"%s (%s)", config->entries[i]->title_show, config->entries[i]->version);
                FreePool(config->entries[i]->title_show);
                config->entries[i]->title_show = s;
                config->entries[i]->non_unique = FALSE;
        }

        unique = TRUE;
        for (i = 0; i < config->entry_count; i++) {
                for (k = 0; k < config->entry_count; k++) {
                        if (i == k)
                                continue;
                        if (StrCmp(config->entries[i]->title_show, config->entries[k]->title_show) != 0)
                                continue;

                        unique = FALSE;
                        config->entries[i]->non_unique = TRUE;
                        config->entries[k]->non_unique = TRUE;
                }
        }
        if (unique)
                return;

        /* add machine-id to non-unique titles */
        for (i = 0; i < config->entry_count; i++) {
                CHAR16 *s;
                CHAR16 *m;

                if (!config->entries[i]->non_unique)
                        continue;
                if (!config->entries[i]->machine_id)
                        continue;

                m = StrDuplicate(config->entries[i]->machine_id);
                m[8] = '\0';
                s = PoolPrint(L"%s (%s)", config->entries[i]->title_show, m);
                FreePool(config->entries[i]->title_show);
                config->entries[i]->title_show = s;
                config->entries[i]->non_unique = FALSE;
                FreePool(m);
        }

        unique = TRUE;
        for (i = 0; i < config->entry_count; i++) {
                for (k = 0; k < config->entry_count; k++) {
                        if (i == k)
                                continue;
                        if (StrCmp(config->entries[i]->title_show, config->entries[k]->title_show) != 0)
                                continue;

                        unique = FALSE;
                        config->entries[i]->non_unique = TRUE;
                        config->entries[k]->non_unique = TRUE;
                }
        }
        if (unique)
                return;

        /* add file name to non-unique titles */
        for (i = 0; i < config->entry_count; i++) {
                CHAR16 *s;

                if (!config->entries[i]->non_unique)
                        continue;
                s = PoolPrint(L"%s (%s)", config->entries[i]->title_show, config->entries[i]->file);
                FreePool(config->entries[i]->title_show);
                config->entries[i]->title_show = s;
                config->entries[i]->non_unique = FALSE;
        }
}

static BOOLEAN config_entry_add_loader(Config *config, EFI_HANDLE *device, EFI_FILE *root_dir, CHAR16 *loaded_image_path,
                                       CHAR16 *file, CHAR16 *title, CHAR16 *loader) {
        EFI_FILE_HANDLE handle;
        EFI_STATUS err;
        ConfigEntry *entry;

        /* do not add an entry for ourselves */
        if (loaded_image_path && StriCmp(loader, loaded_image_path) == 0)
                return FALSE;

        /* check existence */
        err = uefi_call_wrapper(root_dir->Open, 5, root_dir, &handle, loader, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(err))
                return FALSE;
        uefi_call_wrapper(handle->Close, 1, handle);

        entry = AllocateZeroPool(sizeof(ConfigEntry));
        entry->title = StrDuplicate(title);
        entry->device = device;
        entry->loader = StrDuplicate(loader);
        entry->file = StrDuplicate(file);
        StrLwr(entry->file);
        entry->no_autoselect = TRUE;
        config_add_entry(config, entry);
        return TRUE;
}

static VOID config_entry_add_loader_auto(Config *config, EFI_HANDLE *device, EFI_FILE *root_dir, CHAR16 *loaded_image_path,
                                         CHAR16 *file, CHAR16 *title, CHAR16 *loader) {
        if (!config_entry_add_loader(config, device, root_dir, loaded_image_path, file, title, loader))
                return;

        /* export identifiers of automatically added entries */
        if (config->entries_auto) {
                CHAR16 *s;

                s = PoolPrint(L"%s %s", config->entries_auto, file);
                FreePool(config->entries_auto);
                config->entries_auto = s;
        } else
                config->entries_auto = StrDuplicate(file);
}

static VOID config_entry_add_osx(Config *config) {
        EFI_STATUS err;
        UINTN handle_count = 0;
        EFI_HANDLE *handles = NULL;

        err = LibLocateHandle(ByProtocol, &FileSystemProtocol, NULL, &handle_count, &handles);
        if (EFI_ERROR(err) == EFI_SUCCESS) {
                UINTN i;

                for (i = 0; i < handle_count; i++) {
                        EFI_FILE *root;

                        root = LibOpenRoot(handles[i]);
                        if (!root)
                                continue;
                        config_entry_add_loader_auto(config, handles[i], root, NULL, L"auto-osx", L"OS X",
                                                     L"\\System\\Library\\CoreServices\\boot.efi");
                        uefi_call_wrapper(root->Close, 1, root);
                }

                FreePool(handles);
        }
}

static EFI_STATUS image_start(EFI_HANDLE parent_image, const Config *config, const ConfigEntry *entry) {
        EFI_STATUS err;
        EFI_HANDLE image;
        EFI_DEVICE_PATH *path;
        CHAR16 *options;

        path = FileDevicePath(entry->device, entry->loader);
        if (!path) {
                Print(L"Error getting device path.");
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return EFI_INVALID_PARAMETER;
        }

        err = uefi_call_wrapper(BS->LoadImage, 6, FALSE, parent_image, path, NULL, 0, &image);
        if (EFI_ERROR(err)) {
                Print(L"Error loading %s: %r", entry->loader, err);
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                goto out;
        }

        if (config->options_edit)
                options = config->options_edit;
        else if (entry->options)
                options = entry->options;
        else
                options = NULL;
        if (options) {
                EFI_LOADED_IMAGE *loaded_image;

                err = uefi_call_wrapper(BS->OpenProtocol, 6, image, &LoadedImageProtocol, &loaded_image,
                                        parent_image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
                if (EFI_ERROR(err)) {
                        Print(L"Error getting LoadedImageProtocol handle: %r", err);
                        uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                        goto out_unload;
                }
                loaded_image->LoadOptions = options;
                loaded_image->LoadOptionsSize = (StrLen(loaded_image->LoadOptions)+1) * sizeof(CHAR16);
        }

        efivar_set_ticks(L"LoaderTicksExec", 0);
        err = uefi_call_wrapper(BS->StartImage, 3, image, NULL, NULL);
out_unload:
        uefi_call_wrapper(BS->UnloadImage, 1, image);
out:
        FreePool(path);
        return err;
}

static VOID config_free(Config *config) {
        UINTN i;

        for (i = 0; i < config->entry_count; i++)
                config_entry_free(config->entries[i]);
        FreePool(config->entries);
        FreePool(config->entry_default_pattern);
        FreePool(config->options_edit);
        FreePool(config->entries_auto);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *sys_table) {
        EFI_LOADED_IMAGE *loaded_image;
        EFI_FILE *root_dir;
        CHAR16 *loaded_image_path;
        EFI_DEVICE_PATH *device_path;
        EFI_STATUS err;
        Config config;
        UINT64 ticks;
        BOOLEAN menu = FALSE;

        ticks = ticks_read();
        InitializeLib(image, sys_table);
        efivar_set(L"LoaderVersion", L"gummiboot " stringify(VERSION), FALSE);
        efivar_set_ticks(L"LoaderTicksInit", ticks);
        err = uefi_call_wrapper(BS->OpenProtocol, 6, image, &LoadedImageProtocol, &loaded_image,
                                image, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
        if (EFI_ERROR(err)) {
                Print(L"Error getting a LoadedImageProtocol handle: %r ", err);
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return err;
        }

        /* export the device path this image is started from */
        device_path = DevicePathFromHandle(loaded_image->DeviceHandle);
        if (device_path) {
                CHAR16 *str;
                EFI_DEVICE_PATH *path, *paths;

                str = DevicePathToStr(device_path);
                efivar_set(L"LoaderDeviceIdentifier", str, FALSE);
                FreePool(str);

                paths = UnpackDevicePath(device_path);
                for (path = paths; !IsDevicePathEnd(path); path = NextDevicePathNode(path)) {
                        HARDDRIVE_DEVICE_PATH *drive;
                        CHAR16 uuid[37];

                        if (DevicePathType(path) != MEDIA_DEVICE_PATH)
                                continue;
                        if (DevicePathSubType(path) != MEDIA_HARDDRIVE_DP)
                                continue;
                        drive = (HARDDRIVE_DEVICE_PATH *)path;
                        if (drive->SignatureType != SIGNATURE_TYPE_GUID)
                                continue;

                        GuidToString(uuid, (EFI_GUID *)&drive->Signature);
                        efivar_set(L"LoaderDevicePartUUID", uuid, FALSE);
                        break;
                }
                FreePool(paths);
        }

        root_dir = LibOpenRoot(loaded_image->DeviceHandle);
        if (!root_dir) {
                Print(L"Unable to open root directory: %r ", err);
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                return EFI_LOAD_ERROR;
        }

        /* the filesystem path to this image, to prevent adding ourselves to the menu */
        loaded_image_path = DevicePathToStr(loaded_image->FilePath);

        /* scan "\loader\entries\*.conf" files */
        ZeroMem(&config, sizeof(Config));
        config_load(&config, loaded_image->DeviceHandle, root_dir, loaded_image_path);

        /* if we find some well-known loaders, add them to the end of the list */
        config_entry_add_loader_auto(&config, loaded_image->DeviceHandle, root_dir, loaded_image_path,
                                     L"auto-windows", L"Windows Boot Manager", L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
        config_entry_add_loader_auto(&config, loaded_image->DeviceHandle, root_dir, loaded_image_path,
                                     L"auto-efi-shell", L"EFI Shell", L"\\shellx64.efi");
        config_entry_add_loader_auto(&config, loaded_image->DeviceHandle, root_dir, loaded_image_path,
                                     L"auto-efi-default", L"EFI Default Loader", L"\\EFI\\BOOT\\BOOTX64.EFI");
        config_entry_add_osx(&config);
        efivar_set(L"LoaderEntriesAuto", config.entries_auto, FALSE);

        config_title_generate(&config);

        /* select entry by configured pattern or EFI LoaderDefaultEntry= variable*/
        config_default_entry_select(&config);

        if (config.entry_count == 0) {
                Print(L"No loader found. Configuration files in \\loader\\entries\\*.conf are needed.");
                uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                goto out;
        }

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
                        if (!menu_run(&config, &entry, loaded_image_path))
                                break;
                }

                /* export the selected boot entry to the system */
                efivar_set(L"LoaderEntrySelected", entry->file, FALSE);

                err = image_start(image, &config, entry);

                if (err == EFI_ACCESS_DENIED || err == EFI_SECURITY_VIOLATION) {
                        /* Platform is secure boot and requested image isn't
                         * trusted. Need to go back to prior boot system and
                         * install more keys or hashes. Signal failure by
                         * returning the error */
                        Print(L"\nImage %s gives a security error\n", entry->title);
                        Print(L"Please enrol the hash or signature of %s\n", entry->loader);
                        uefi_call_wrapper(BS->Stall, 1, 3 * 1000 * 1000);
                        goto out;
                }

                menu = TRUE;
                config.timeout_sec = 0;
        }
        err = EFI_SUCCESS;
out:
        FreePool(loaded_image_path);
        config_free(&config);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        uefi_call_wrapper(BS->CloseProtocol, 4, image, &LoadedImageProtocol, image, NULL);
        return err;
}
