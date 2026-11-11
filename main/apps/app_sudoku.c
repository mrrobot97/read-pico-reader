/* SPDX-License-Identifier: Apache-2.0
 * 经典数独，作为设置的子页面打开。/ Classic Sudoku, opened from Settings.
 */
#include <string.h>

#include "app.h"
#include "esp_random.h"
#include "kiiko_board_config.h"
#include "nvs.h"
#include "sudoku_generator.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"

enum {
    SIDE = SUDOKU_SIDE, CELLS = SUDOKU_CELLS, DIFFICULTIES = SUDOKU_LEVELS, PUZZLES_PER_LEVEL = 12,
    NOTE_MASK = 0x1ff, SAVE_VERSION = 2, SAVE_DELAY_MS = 2500, UNDO_LIMIT = 32,
    GENERATION_ATTEMPTS = 12,
    HEADER_X = 36, HEADER_Y = 79, HEADER_HEIGHT = 68,
    LEVEL_Y = HEADER_Y + HEADER_HEIGHT + 10, LEVEL_HEIGHT = 54,
    GRID_X = 36, GRID_Y = LEVEL_Y + LEVEL_HEIGHT + 13,
    CELL_SIZE = 68, GRID_SIZE = SIDE * CELL_SIZE,
};

static const sudoku_puzzle_t puzzles[DIFFICULTIES][PUZZLES_PER_LEVEL] = {
#include "sudoku_puzzles.inc"
};

typedef struct {
    uint16_t notes[CELLS];
    uint8_t entries[CELLS];
    uint8_t version, level, legacy_index;
} sudoku_progress_t;

_Static_assert(sizeof(sudoku_progress_t) == 246, "Keep the version 1 Sudoku save layout");

typedef struct {
    sudoku_progress_t progress;
    sudoku_puzzle_t puzzle;
} sudoku_save_t;

typedef struct {
    uint16_t notes;
    uint8_t cell, entry;
} sudoku_move_t;

static sudoku_progress_t s_game;
static sudoku_puzzle_t s_puzzle;
static sudoku_move_t s_undo[UNDO_LIMIT];
static unsigned s_undo_next, s_undo_count;
static int s_selected = -1, s_pending_level = -1;
static int s_generate_level = -1;
static unsigned s_generate_attempts;
static bool s_loaded, s_dirty, s_save_failed, s_notes_mode, s_checked, s_complete;
static int64_t s_save_due;
static EpdRect s_area;

static const EpdRect board = {GRID_X, GRID_Y, GRID_SIZE, GRID_SIZE};
static const EpdRect back_button = {HEADER_X - 10, HEADER_Y + 3, 64, 64};
static const EpdRect status_area = {36, 847, 612, 52};
static const EpdRect controls = {36, 1006, 612, 76};
static const char *const levels[] = {"入门", "进阶", "挑战"};
static const char *const actions[] = {"候选数", "清除", "撤销", "检查", "新局"};

static bool valid_save(const sudoku_save_t *save) {
    const sudoku_progress_t *progress = &save->progress;
    if (progress->version != SAVE_VERSION || progress->level >= DIFFICULTIES ||
        !sudoku_puzzle_valid(&save->puzzle)) return false;
    for (int i = 0; i < CELLS; ++i) {
        const uint8_t entry = progress->entries[i];
        const uint16_t notes = progress->notes[i];
        if (entry > 9 || (notes & ~NOTE_MASK) || (notes && entry) ||
            (save->puzzle.givens[i] != '0' && (entry || notes))) return false;
    }
    return true;
}

static void save_game(app_ctx_t *ctx) {
    (void)ctx;
    if (!s_dirty) return;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KIIKO_BOARD_SETTINGS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        const sudoku_save_t saved = {s_game, s_puzzle};
        err = nvs_set_blob(handle, "sudoku", &saved, sizeof(saved));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    s_save_failed = s_dirty = err != ESP_OK;
    s_save_due = 0;
}

static void mark_dirty(app_ctx_t *ctx) {
    s_dirty = true;
    s_save_due = ctx->now_ms + SAVE_DELAY_MS;
}

static void update_completion(void) {
    s_complete = true;
    for (int i = 0; i < CELLS; ++i) {
        if (s_puzzle.givens[i] == '0' && s_game.entries[i] != s_puzzle.solution[i] - '0') {
            s_complete = false;
            break;
        }
    }
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_pending_level = -1;
    if (s_loaded) return;
    s_game = (sudoku_progress_t){.version = SAVE_VERSION};
    sudoku_shuffle_puzzle(&s_puzzle, &puzzles[0][esp_random() % PUZZLES_PER_LEVEL], esp_random());
    nvs_handle_t handle;
    if (nvs_open(KIIKO_BOARD_SETTINGS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        sudoku_save_t saved = {0};
        size_t size = sizeof(saved);
        if (nvs_get_blob(handle, "sudoku", &saved, &size) == ESP_OK) {
            sudoku_progress_t *progress = &saved.progress;
            // 旧存档按题号取棋盘。/ The original save stored the built-in puzzle index.
            if (size == sizeof(*progress) && progress->version == 1 &&
                progress->level < DIFFICULTIES && progress->legacy_index < PUZZLES_PER_LEVEL) {
                saved.puzzle = puzzles[progress->level][progress->legacy_index];
                progress->version = SAVE_VERSION;
                progress->legacy_index = 0;
                size = sizeof(saved);
            }
            if (size == sizeof(saved) && valid_save(&saved)) {
                s_game = saved.progress;
                s_puzzle = saved.puzzle;
            }
        }
        nvs_close(handle);
    }
    update_completion();
    s_loaded = true;
}

static void leave_game(app_ctx_t *ctx) {
    s_generate_level = -1;
    save_game(ctx);
}

static EpdRect level_rect(int i) { return (EpdRect){36 + i * 208, LEVEL_Y, 196, LEVEL_HEIGHT}; }
static EpdRect digit_rect(int i) { return (EpdRect){36 + i * 69, 914, 60, 76}; }
static EpdRect action_rect(int i) { return (EpdRect){36 + i * 125, 1006, 112, 76}; }
static EpdRect confirm_rect(int i) { return (EpdRect){36 + i * 312, 1006, 300, 76}; }

static EpdRect cell_rect(int cell) {
    return (EpdRect){GRID_X + (cell % SIDE) * CELL_SIZE + 3,
                     GRID_Y + (cell / SIDE) * CELL_SIZE + 3,
                     CELL_SIZE - 6, CELL_SIZE - 6};
}

static bool incorrect(int cell) {
    return s_game.entries[cell] && s_game.entries[cell] != s_puzzle.solution[cell] - '0';
}

static void draw_cell(uint8_t *fb, int cell) {
    const EpdRect r = cell_rect(cell);
    const bool selected = cell == s_selected;
    const char given = s_puzzle.givens[cell];
    ui_fill_rect_fast(r, selected ? UI_GRAY_BLACK : UI_GRAY_WHITE, fb);
    const char digit = given != '0' ? given : '0' + s_game.entries[cell];
    if (digit != '0') {
        const char text[] = {digit, 0};
        const EpdRect glyph = {r.x + 5, r.y + 8, r.width - 10, r.height - 16};
        ui_text_box(fb, glyph, given != '0' ? 42 : 36, text, EPD_DRAW_ALIGN_CENTER, selected);
        if (given != '0') {
            ui_fill_rect_fast((EpdRect){r.x + 4, r.y + 4, 5, 5},
                              selected ? UI_GRAY_WHITE : UI_GRAY_BLACK, fb);
        } else if (s_checked && incorrect(cell)) {
            ui_fill_rect_fast((EpdRect){r.x + 16, r.y + r.height - 5, r.width - 32, 2},
                              selected ? UI_GRAY_WHITE : UI_GRAY_BLACK, fb);
        }
    } else {
        for (int n = 0; n < SIDE; ++n) {
            if (!(s_game.notes[cell] & (1u << n))) continue;
            const char text[] = {'1' + n, 0};
            const EpdRect slot = {r.x + 1 + (n % 3) * 20, r.y + 1 + (n / 3) * 20, 20, 20};
            ui_text_box(fb, slot, 15, text, EPD_DRAW_ALIGN_CENTER, selected);
        }
    }
    ui_image_bw_rect(fb, r);
}

static void draw_button(uint8_t *fb, EpdRect r, const char *label, bool active) {
    ui_fill_round_rect(fb, r, 12, active ? UI_GRAY_BLACK : UI_GRAY_WHITE);
    ui_draw_control_frame(fb, r, 12, UI_GRAY_BLACK);
    ui_text_box(fb, (EpdRect){r.x + 6, r.y + 5, r.width - 12, r.height - 10},
                26, label, EPD_DRAW_ALIGN_CENTER, active);
    ui_image_bw_rect(fb, r);
}

static void draw_status(uint8_t *fb) {
    const char *label = "点选格子，再填数字";
    if (s_generate_level >= 0) label = "正在生成题目…";
    else if (s_pending_level >= 0) label = "开始新局会替换当前进度";
    else if (s_save_failed) label = "数独进度保存失败";
    else if (s_complete) label = "解题成功！";
    else if (s_checked) {
        label = "目前填写正确";
        for (int i = 0; i < CELLS; ++i) {
            if (!incorrect(i)) continue;
            label = "带下划线的数字有误";
            break;
        }
    }
    ui_fill_rect_fast(status_area, UI_GRAY_WHITE, fb);
    ui_text_box(fb, status_area, 26, label, EPD_DRAW_ALIGN_CENTER, false);
    ui_image_bw_rect(fb, status_area);
}

static void draw_controls(uint8_t *fb) {
    ui_fill_rect_fast(controls, UI_GRAY_WHITE, fb);
    if (s_generate_level >= 0 || s_pending_level >= 0) {
        draw_button(fb, confirm_rect(0), "取消", false);
        if (s_pending_level >= 0) draw_button(fb, confirm_rect(1), "开始新局", true);
    } else {
        for (int i = 0; i < 5; ++i) {
            draw_button(fb, action_rect(i), actions[i], i == 0 && s_notes_mode);
        }
    }
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_fill_rect_fast((EpdRect){0, 0, UI_LOCK_WIDTH, UI_LOCK_HEIGHT}, UI_GRAY_WHITE, fb);
    ui_nav_status(fb);
    ui_nav_back(fb, HEADER_X, HEADER_Y);
    ui_text_box(fb, (EpdRect){130, HEADER_Y, 424, HEADER_HEIGHT}, 44, "数独", EPD_DRAW_ALIGN_CENTER, false);
    for (int i = 0; i < DIFFICULTIES; ++i) draw_button(fb, level_rect(i), levels[i], i == s_game.level);
    for (int i = 0; i <= SIDE; ++i) {
        const int width = i % 3 ? 1 : 3;
        const int offset = i == SIDE ? GRID_SIZE - width : i * CELL_SIZE;
        ui_fill_rect_fast((EpdRect){GRID_X + offset, GRID_Y, width, GRID_SIZE}, UI_GRAY_BLACK, fb);
        ui_fill_rect_fast((EpdRect){GRID_X, GRID_Y + offset, GRID_SIZE, width}, UI_GRAY_BLACK, fb);
    }
    for (int i = 0; i < CELLS; ++i) draw_cell(fb, i);
    draw_status(fb);
    for (int i = 0; i < SIDE; ++i) {
        const char text[] = {'1' + i, 0};
        draw_button(fb, digit_rect(i), text, false);
    }
    draw_controls(fb);
    ui_text_box(fb, (EpdRect){36, 1112, 612, 48}, 24, "每行、每列、每宫填入 1–9", EPD_DRAW_ALIGN_CENTER, false);
}

static EpdRect redraw_cell(uint8_t *fb, EpdRect area, int cell) {
    if (cell < 0) return area;
    draw_cell(fb, cell);
    return ui_rect_union(area, cell_rect(cell));
}

static void redraw_incorrect_cells(uint8_t *fb) {
    for (int i = 0; i < CELLS; ++i) {
        if (incorrect(i)) s_area = redraw_cell(fb, s_area, i);
    }
}

static app_redraw_t select_cell(app_ctx_t *ctx, int cell) {
    if (cell == s_selected) return APP_REDRAW_NONE;
    const int old = s_selected;
    s_selected = cell;
    s_area = redraw_cell(ctx->fb, cell_rect(cell), old);
    draw_cell(ctx->fb, cell);
    return APP_REDRAW_AREA;
}

static app_redraw_t after_edit(app_ctx_t *ctx, int old_cell) {
    mark_dirty(ctx);
    const bool was_complete = s_complete;
    const bool was_checked = s_checked;
    s_checked = false;
    update_completion();
    s_area = redraw_cell(ctx->fb, cell_rect(s_selected), old_cell);
    draw_cell(ctx->fb, s_selected);
    // 改动的格子已重画，清除其他格子的旧检查标记。/ Clear stale marks on the other cells after an edit.
    if (was_checked) redraw_incorrect_cells(ctx->fb);
    if (was_checked || was_complete != s_complete) {
        draw_status(ctx->fb);
        s_area = ui_rect_union(s_area, status_area);
    }
    return APP_REDRAW_AREA;
}

static app_redraw_t edit_cell(app_ctx_t *ctx, int digit) {
    const int cell = s_selected;
    if (s_complete || cell < 0 || s_puzzle.givens[cell] != '0')
        return APP_REDRAW_NONE;
    uint8_t entry = digit;
    uint16_t notes = 0;
    if (s_notes_mode && digit) {
        if (s_game.entries[cell]) return APP_REDRAW_NONE;
        entry = 0;
        notes = s_game.notes[cell] ^ (1u << (digit - 1));
    }
    if (entry == s_game.entries[cell] && notes == s_game.notes[cell]) return APP_REDRAW_NONE;
    s_undo[s_undo_next] = (sudoku_move_t){s_game.notes[cell], cell, s_game.entries[cell]};
    s_undo_next = (s_undo_next + 1) % UNDO_LIMIT;
    if (s_undo_count < UNDO_LIMIT) ++s_undo_count;
    s_game.entries[cell] = entry;
    s_game.notes[cell] = notes;
    return after_edit(ctx, -1);
}

static app_redraw_t undo_move(app_ctx_t *ctx) {
    if (!s_undo_count) return APP_REDRAW_NONE;
    --s_undo_count;
    s_undo_next = (s_undo_next + UNDO_LIMIT - 1) % UNDO_LIMIT;
    const sudoku_move_t move = s_undo[s_undo_next];
    const int old = s_selected;
    s_selected = move.cell;
    s_game.entries[move.cell] = move.entry;
    s_game.notes[move.cell] = move.notes;
    return after_edit(ctx, old == s_selected ? -1 : old);
}

static app_redraw_t install_puzzle(app_ctx_t *ctx, int level, const sudoku_puzzle_t *puzzle) {
    s_game = (sudoku_progress_t){.version = SAVE_VERSION, .level = level};
    s_puzzle = *puzzle;
    s_selected = s_pending_level = -1;
    s_generate_level = -1;
    s_undo_next = s_undo_count = 0;
    s_notes_mode = s_checked = s_complete = false;
    mark_dirty(ctx);
    return APP_REDRAW_FULL;
}

static app_redraw_t redraw_controls(app_ctx_t *ctx) {
    draw_status(ctx->fb);
    draw_controls(ctx->fb);
    s_area = ui_rect_union(status_area, controls);
    return APP_REDRAW_AREA;
}

static app_redraw_t start_generation(app_ctx_t *ctx, int level) {
    s_pending_level = -1;
    s_generate_level = level;
    s_generate_attempts = 0;
    return redraw_controls(ctx);
}

static app_redraw_t request_new_game(app_ctx_t *ctx, int level) {
    if (!s_complete) {
        for (int i = 0; i < CELLS; ++i) {
            if (s_game.entries[i] || s_game.notes[i]) {
                s_pending_level = level;
                return redraw_controls(ctx);
            }
        }
    }
    return start_generation(ctx, level);
}

static app_redraw_t go_back(app_ctx_t *ctx) {
    if (s_pending_level >= 0 || s_generate_level >= 0) {
        s_pending_level = s_generate_level = -1;
        return redraw_controls(ctx);
    }
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    const int x = event->x0, y = event->y0;
    if (ui_rect_hit(back_button, x, y)) return go_back(ctx);
    if (s_pending_level >= 0 || s_generate_level >= 0) {
        if (ui_rect_hit(confirm_rect(0), x, y)) return go_back(ctx);
        if (s_pending_level >= 0 && ui_rect_hit(confirm_rect(1), x, y))
            return start_generation(ctx, s_pending_level);
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(board, x, y)) return select_cell(ctx, (y - GRID_Y) / CELL_SIZE * SIDE + (x - GRID_X) / CELL_SIZE);
    for (int i = 0; i < DIFFICULTIES; ++i) {
        if (i != s_game.level && ui_rect_hit(level_rect(i), x, y)) return request_new_game(ctx, i);
    }
    for (int i = 0; i < SIDE; ++i) {
        if (ui_rect_hit(digit_rect(i), x, y)) return edit_cell(ctx, i + 1);
    }
    if (ui_rect_hit(action_rect(0), x, y)) {
        s_notes_mode = !s_notes_mode;
        s_area = action_rect(0);
        draw_button(ctx->fb, s_area, actions[0], s_notes_mode);
        return APP_REDRAW_AREA;
    }
    if (ui_rect_hit(action_rect(1), x, y)) return edit_cell(ctx, 0);
    if (ui_rect_hit(action_rect(2), x, y)) return undo_move(ctx);
    if (ui_rect_hit(action_rect(3), x, y)) {
        s_checked = true;
        s_area = status_area;
        redraw_incorrect_cells(ctx->fb);
        draw_status(ctx->fb);
        return APP_REDRAW_AREA;
    }
    if (ui_rect_hit(action_rect(4), x, y)) return request_new_game(ctx, s_game.level);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    return key == UI_KEY_1 ? go_back(ctx) : APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (s_generate_level >= 0) {
        const int level = s_generate_level;
        sudoku_puzzle_t next;
        // 每轮只尝试一次，用户可取消，确认新题后才替换当前进度。
        // Try once per tick and keep the current game until a new puzzle is ready.
        if (sudoku_generate(&next, level, esp_random()) && strcmp(s_puzzle.givens, next.givens))
            return install_puzzle(ctx, level, &next);
        if (++s_generate_attempts < GENERATION_ATTEMPTS) return APP_REDRAW_NONE;
        const sudoku_puzzle_t *fallback = &puzzles[level][esp_random() % PUZZLES_PER_LEVEL];
        sudoku_shuffle_puzzle(&next, fallback, esp_random());
        return install_puzzle(ctx, level, &next);
    }
    if (!s_save_due || ctx->now_ms < s_save_due) return APP_REDRAW_NONE;
    const bool failed = s_save_failed;
    save_game(ctx);
    if (failed == s_save_failed) return APP_REDRAW_NONE;
    draw_status(ctx->fb);
    s_area = status_area;
    return APP_REDRAW_AREA;
}

static EpdRect area_hint(app_ctx_t *ctx) { (void)ctx; return s_area; }

const app_desc_t app_sudoku = {
    .title = "数独",
    .detail = "休闲游戏",
    .enter_full = true,
    .on_enter = on_enter,
    .on_exit = leave_game,
    .on_before_lock = save_game,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
