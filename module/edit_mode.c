#include "edit_mode.h"

#include <string.h>

// this
#include "flash.h"
#include "globals.h"
#include "keyboard_helper.h"
#include "line_editor.h"

// libavr32
#include "font.h"
#include "kbd.h"
#include "region.h"
#include "util.h"

// asf
#include "conf_usb_host.h"  // needed in order to include "usb_protocol_hid.h"
#include "usb_protocol_hid.h"

static line_editor_t le;
static uint8_t line_no1 = 0, line_no2 = 0;
static uint8_t script;
static error_t status;
static char error_msg[TELE_ERROR_MSG_LENGTH];

static const uint8_t D_INPUT = 1 << 0;
static const uint8_t D_LIST = 1 << 1;
static const uint8_t D_MESSAGE = 1 << 2;
static const uint8_t D_ALL = 0xFF;
static uint8_t dirty;

void set_edit_mode() {
    status = E_OK;
    error_msg[0] = 0;
    line_no2 = line_no1 = 0;
    line_editor_set_command(
        &le, ss_get_script_command(&scene_state, script, line_no1));
    dirty = D_ALL;
}

void set_edit_mode_script(uint8_t new_script) {
    script = new_script;
    if (script >= SCRIPT_COUNT) script = SCRIPT_COUNT - 2;
    dirty = D_ALL;
}

void process_edit_keys(uint8_t k, uint8_t m, bool is_held_key) {
    // <down> or C-n: line down
    if (match_no_mod(m, k, HID_DOWN) || match_ctrl(m, k, HID_N)) {
        if (line_no1 < (SCRIPT_MAX_COMMANDS - 1) &&
            line_no1 < ss_get_script_len(&scene_state, script)) {
            line_no1++;
            line_editor_set_command(
                &le, ss_get_script_command(&scene_state, script, line_no1));
            dirty |= D_LIST | D_INPUT;
        }
        line_no2 = line_no1;
    }
    // <up> or C-p: line up
    else if (match_no_mod(m, k, HID_UP) || match_ctrl(m, k, HID_P)) {
        if (line_no1) {
            line_no1--;
            line_editor_set_command(
                &le, ss_get_script_command(&scene_state, script, line_no1));
            dirty |= D_LIST | D_INPUT;
        }
        line_no2 = line_no1;
    }
    // shift-<down> or C-S-n: expand selection down
    else if (match_shift(m, k, HID_DOWN) || match_shift_ctrl(m, k, HID_N)) {
        if (line_no2 < (SCRIPT_MAX_COMMANDS - 1) &&
            line_no2 < ss_get_script_len(&scene_state, script) - 1) {
            line_no2++;
            dirty |= D_LIST | D_INPUT;
        }
    }
    // shift-<up> or C-S-p: expand selection up
    else if (match_shift(m, k, HID_UP) || match_shift_ctrl(m, k, HID_P)) {
        if (line_no2) {
            line_no2--;
            dirty |= D_LIST | D_INPUT;
        }
    }
    // [: previous script
    else if (match_no_mod(m, k, HID_OPEN_BRACKET)) {
        status = E_OK;
        error_msg[0] = 0;
        if (script)
            script--;
        else
            script = SCRIPT_COUNT - 2;  // due to TEMP_SCRIPT
        if (line_no1 > ss_get_script_len(&scene_state, script))
            line_no1 = ss_get_script_len(&scene_state, script);
        line_editor_set_command(
            &le, ss_get_script_command(&scene_state, script, line_no1));
        line_no2 = line_no1;
        dirty |= D_LIST | D_INPUT;
    }
    // ]: next script
    else if (match_no_mod(m, k, HID_CLOSE_BRACKET)) {
        status = E_OK;
        error_msg[0] = 0;
        script++;
        if (script >= SCRIPT_COUNT - 1) script = 0;  // due to TEMP_SCRIPT
        if (line_no1 > ss_get_script_len(&scene_state, script))
            line_no1 = ss_get_script_len(&scene_state, script);
        line_editor_set_command(
            &le, ss_get_script_command(&scene_state, script, line_no1));
        line_no2 = line_no1;
        dirty |= D_LIST | D_INPUT;
    }
    // alt-<down>: move selected lines down
    else if (match_alt(m, k, HID_DOWN)) {
        u8 l1 = min(line_no1, line_no2);
        u8 l2 = max(line_no1, line_no2);
        if (l2 < SCRIPT_MAX_COMMANDS - 1 &&
            l2 < ss_get_script_len(&scene_state, script) - 1) {
            tele_command_t temp;
            ss_copy_script_command(&temp, &scene_state, script, l2 + 1);
            ss_delete_script_command(&scene_state, script, l2 + 1);
            ss_insert_script_command(&scene_state, script, l1, &temp);
            line_no1++;
            line_no2++;
            dirty |= D_LIST | D_INPUT;
        }
    }
    // alt-<up>: move selected lines up
    else if (match_alt(m, k, HID_UP)) {
        u8 l1 = min(line_no1, line_no2);
        u8 l2 = max(line_no1, line_no2);
        if (l1) {
            tele_command_t temp;
            ss_copy_script_command(&temp, &scene_state, script, l1 - 1);
            ss_delete_script_command(&scene_state, script, l1 - 1);
            ss_insert_script_command(&scene_state, script, l2, &temp);
            line_no1--;
            line_no2--;
            dirty |= D_LIST | D_INPUT;
        }
    }
    // ctrl-x or alt-x: override line editors cut
    else if (match_ctrl(m, k, HID_X) || match_alt(m, k, HID_X)) {
        if (line_no1 == line_no2) {
            strcpy(copy_buffer[0], line_editor_get(&le));
            copy_buffer_len = 1;
            ss_delete_script_command(&scene_state, script, line_no1);
        }
        else {
            copy_buffer_len = 0;
            for (u8 l = min(line_no1, line_no2); l <= max(line_no1, line_no2);
                 l++)
                print_command(ss_get_script_command(&scene_state, script, l),
                              copy_buffer[copy_buffer_len++]);
            for (s8 l = max(line_no1, line_no2); l >= min(line_no1, line_no2);
                 l--)
                ss_delete_script_command(&scene_state, script, l);
        }

        if (line_no1 > ss_get_script_len(&scene_state, script)) {
            line_no1 = ss_get_script_len(&scene_state, script);
        }
        line_editor_set_command(
            &le, ss_get_script_command(&scene_state, script, line_no1));
        line_no2 = line_no1;

        dirty |= D_LIST | D_INPUT;
    }
    // ctrl-c or alt-c: override line editors copy for multi selection
    else if (match_ctrl(m, k, HID_C) || match_alt(m, k, HID_C)) {
        if (line_no1 == line_no2) {
            bool processed = line_editor_process_keys(&le, k, m, is_held_key);
            if (processed) dirty |= D_INPUT;
            return;
        }

        copy_buffer_len = 0;
        for (u8 l = min(line_no1, line_no2); l <= max(line_no1, line_no2); l++)
            print_command(ss_get_script_command(&scene_state, script, l),
                          copy_buffer[copy_buffer_len++]);
    }
    // ctrl-v or alt-v: override line editors paste for multi selection
    else if (match_ctrl(m, k, HID_V) || match_alt(m, k, HID_V)) {
        if (copy_buffer_len == 0) return;

        u8 idx = min(line_no1, line_no2);
        tele_command_t command;
        for (u8 i = 0; i < copy_buffer_len; i++) {
            if (parse(copy_buffer[i], &command, error_msg) != E_OK) continue;
            if (validate(&command, error_msg) != E_OK) continue;
            if (command.length == 0) continue;

            ss_insert_script_command(&scene_state, script, idx++, &command);
            if (line_no1 < (SCRIPT_MAX_COMMANDS - 1)) line_no1++;
            if (line_no2 < (SCRIPT_MAX_COMMANDS - 1)) line_no2++;
            if (idx >= SCRIPT_MAX_COMMANDS) break;
        }
        line_editor_set_command(
            &le, ss_get_script_command(&scene_state, script, line_no1));
        dirty |= D_LIST | D_INPUT;
    }
    // alt-delete: remove selected lines
    else if (match_alt(m, k, HID_DELETE)) {
        for (s8 l = max(line_no1, line_no2); l >= min(line_no1, line_no2); l--)
            ss_delete_script_command(&scene_state, script, l);
        if (line_no1 > ss_get_script_len(&scene_state, script))
            line_no1 = ss_get_script_len(&scene_state, script);
        line_editor_set_command(
            &le, ss_get_script_command(&scene_state, script, line_no1));
        line_no2 = line_no1;
        dirty |= D_LIST | D_INPUT;
    }
    // <enter>: enter command
    else if (match_no_mod(m, k, HID_ENTER)) {
        if (line_no1 != line_no2) {
            line_no2 = line_no1;
            line_editor_set_command(
                &le, ss_get_script_command(&scene_state, script, line_no1));
            dirty |= D_LIST | D_INPUT;
            return;
        }

        dirty |= D_MESSAGE;  // something will happen

        tele_command_t command;
        status = parse(line_editor_get(&le), &command, error_msg);

        if (status != E_OK)
            return;  // quit, screen_refresh_edit will display the error message

        status = validate(&command, error_msg);
        if (status != E_OK)
            return;  // quit, screen_refresh_edit will display the error message

        if (command.length == 0) {  // blank line, delete the command
            ss_delete_script_command(&scene_state, script, line_no1);
            if (line_no1 > ss_get_script_len(&scene_state, script)) {
                line_no1 = ss_get_script_len(&scene_state, script);
            }
        }
        else {
            ss_overwrite_script_command(&scene_state, script, line_no1,
                                        &command);
            if (line_no1 < SCRIPT_MAX_COMMANDS - 1) { line_no1++; }
        }
        line_editor_set_command(
            &le, ss_get_script_command(&scene_state, script, line_no1));
        line_no2 = line_no1;
        dirty |= D_LIST | D_INPUT;
    }
    // shift-<enter>: insert command
    else if (match_shift(m, k, HID_ENTER)) {
        if (line_no1 != line_no2) {
            line_no2 = line_no1;
            line_editor_set_command(
                &le, ss_get_script_command(&scene_state, script, line_no1));
            dirty |= D_LIST | D_INPUT;
            return;
        }

        dirty |= D_MESSAGE;  // something will happen

        tele_command_t command;
        status = parse(line_editor_get(&le), &command, error_msg);

        if (status != E_OK)
            return;  // quit, screen_refresh_edit will display the error message

        status = validate(&command, error_msg);
        if (status != E_OK)
            return;  // quit, screen_refresh_edit will display the error message

        if (command.length > 0) {
            ss_insert_script_command(&scene_state, script, line_no1, &command);
            if (line_no1 < (SCRIPT_MAX_COMMANDS - 1)) { line_no1++; }
        }

        line_editor_set_command(
            &le, ss_get_script_command(&scene_state, script, line_no1));
        line_no2 = line_no1;
        dirty |= D_LIST | D_INPUT;
    }
    // alt-slash comment toggle selected lines
    else if (match_alt(m, k, HID_SLASH)) {
        for (u8 l = min(line_no1, line_no2); l <= max(line_no1, line_no2); l++)
            ss_toggle_script_comment(&scene_state, script, l);
        dirty |= D_LIST;
    }
    else {  // pass the key though to the line editor
        if (line_no1 != line_no2) {
            line_no2 = line_no1;
            line_editor_set_command(
                &le, ss_get_script_command(&scene_state, script, line_no1));
            dirty |= D_LIST | D_INPUT;
            return;
        }

        bool processed = line_editor_process_keys(&le, k, m, is_held_key);
        if (processed) dirty |= D_INPUT;
    }
}

void screen_mutes_updated() {
    dirty |= D_INPUT;
}

uint8_t screen_refresh_edit() {
    uint8_t screen_dirty = 0;
    u8 sel1 = min(line_no1, line_no2);
    u8 sel2 = max(line_no1, line_no2);

    if (dirty & D_INPUT) {
        char prefix = script + '1';
        if (script == METRO_SCRIPT)
            prefix = 'M';
        else if (script == INIT_SCRIPT)
            prefix = 'I';

        if (sel1 == sel2)
            line_editor_draw(&le, prefix, &line[7]);
        else
            region_fill(&line[7], 0);

        char script_no[2] = { prefix, '\0' };
        font_string_region_clip(&line[7], script_no, 0, 0,
                                ss_get_mute(&scene_state, script) ? 4 : 15, 0);

        screen_dirty |= (1 << 7);
        dirty &= ~D_INPUT;
    }

    if (dirty & D_MESSAGE) {
        char s[32];
        if (status != E_OK) {
            strcpy(s, tele_error(status));
            if (error_msg[0]) {
                size_t len = strlen(s);
                strcat(s, ": ");
                strncat(s, error_msg, 32 - len - 3);
                error_msg[0] = 0;
            }
            status = E_OK;
        }
        else {
            s[0] = 0;
        }

        region_fill(&line[6], 0);
        font_string_region_clip(&line[6], s, 0, 0, 0x4, 0);

        screen_dirty |= (1 << 6);
        dirty &= ~D_MESSAGE;
    }

    char s[32];
    if (dirty & D_LIST) {
        for (int i = 0; i < 6; i++) {
            uint8_t a = i >= sel1 && i <= sel2;
            uint8_t fg =
                ss_get_script_comment(&scene_state, script, i) ? 0x7 : 0xf;
            region_fill(&line[i], a);
            if (ss_get_script_len(&scene_state, script) > i) {
                print_command(ss_get_script_command(&scene_state, script, i),
                              s);
                region_string(&line[i], s, 2, 0, fg, a, 0);
            }
        }

        screen_dirty |= 0x3F;
        dirty &= ~D_LIST;
    }

    return screen_dirty;
}
