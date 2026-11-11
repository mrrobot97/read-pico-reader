/* SPDX-License-Identifier: Apache-2.0 */
#include "sudoku_generator.h"

#include <string.h>

enum { SIDE = SUDOKU_SIDE, CELLS = SUDOKU_CELLS, ALL_DIGITS = 0x1ff, SEARCH_BUDGET = 3600 };

typedef struct {
    uint16_t rows[SIDE], columns[SIDE], boxes[SIDE];
    uint8_t digits[CELLS];
} grid_t;

static uint32_t random_next(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void shuffle(uint8_t *values, unsigned count, uint32_t *state) {
    for (unsigned i = 0; i < count; ++i) values[i] = i;
    for (unsigned i = count - 1; i > 0; --i) {
        const unsigned j = random_next(state) % (i + 1);
        const uint8_t value = values[i];
        values[i] = values[j];
        values[j] = value;
    }
}

static void shuffle_axis(uint8_t axis[SIDE], uint32_t *state) {
    uint8_t groups[3], within[3];
    shuffle(groups, 3, state);
    for (int group = 0; group < 3; ++group) {
        shuffle(within, 3, state);
        for (int i = 0; i < 3; ++i) axis[group * 3 + i] = groups[group] * 3 + within[i];
    }
}

static void permute_puzzle(sudoku_puzzle_t *out, const sudoku_puzzle_t *puzzle, uint32_t *state) {
    uint8_t rows[SIDE], columns[SIDE], digits[SIDE];
    shuffle_axis(rows, state);
    shuffle_axis(columns, state);
    shuffle(digits, SIDE, state);
    for (int i = 0; i < CELLS; ++i) {
        const int row = rows[i / SIDE], col = columns[i % SIDE];
        const int source = row * SIDE + col;
        const int value = puzzle ? puzzle->solution[source] - '1' : (row * 3 + row / 3 + col) % SIDE;
        out->solution[i] = '1' + digits[value];
        out->givens[i] = puzzle && puzzle->givens[source] == '0' ? '0' : out->solution[i];
    }
    out->givens[CELLS] = out->solution[CELLS] = '\0';
}

void sudoku_shuffle_puzzle(sudoku_puzzle_t *out, const sudoku_puzzle_t *puzzle, uint32_t seed) {
    if (!seed) seed = 1;
    permute_puzzle(out, puzzle, &seed);
}

static int box_index(int cell) { return cell / 27 * 3 + cell % SIDE / 3; }

static unsigned candidates(const grid_t *grid, int cell) {
    return ALL_DIGITS & ~(grid->rows[cell / SIDE] | grid->columns[cell % SIDE] | grid->boxes[box_index(cell)]);
}

// 搜索回溯使用同一操作撤回数字及占用位。/ Backtracking applies the same operation to undo a placement.
static void toggle_digit(grid_t *grid, int cell, unsigned bit) {
    grid->rows[cell / SIDE] ^= bit;
    grid->columns[cell % SIDE] ^= bit;
    grid->boxes[box_index(cell)] ^= bit;
    grid->digits[cell] ^= (unsigned)__builtin_ctz(bit) + 1;
}

static bool read_grid(grid_t *grid, const char *digits) {
    memset(grid, 0, sizeof(*grid));
    for (int cell = 0; cell < CELLS; ++cell) {
        const int digit = digits[cell] - '0';
        if (digit == 0) continue;
        if (digit < 1 || digit > SIDE) return false;
        const unsigned bit = 1u << (digit - 1);
        if (!(candidates(grid, cell) & bit)) return false;
        toggle_digit(grid, cell, bit);
    }
    return digits[CELLS] == '\0';
}

bool sudoku_puzzle_valid(const sudoku_puzzle_t *puzzle) {
    bool has_blank = false;
    if (puzzle->givens[CELLS] != '\0') return false;
    for (int i = 0; i < CELLS; ++i) {
        if (puzzle->solution[i] < '1' || puzzle->solution[i] > '9') return false;
        if (puzzle->givens[i] == '0') has_blank = true;
        else if (puzzle->givens[i] != puzzle->solution[i]) return false;
    }
    grid_t grid;
    return has_blank && read_grid(&grid, puzzle->solution);
}

static unsigned first_bit(unsigned mask) { return mask & (0u - mask); }

/* Stop on the second solution, or reject the removal if the search budget is exhausted. */
static bool has_unique_solution(const char *digits, unsigned *budget) {
    grid_t grid;
    if (!read_grid(&grid, digits)) return false;
    struct { uint16_t remaining, bit; uint8_t cell; } stack[CELLS];
    int depth = 0;
    bool found = false;
    for (;;) {
        if (!*budget) return false;
        --*budget;
        int cell = -1, fewest = SIDE + 1;
        unsigned options = 0;
        for (int i = 0; i < CELLS; ++i) {
            if (grid.digits[i]) continue;
            const unsigned mask = candidates(&grid, i);
            const int count = __builtin_popcount(mask);
            if (count >= fewest) continue;
            cell = i;
            options = mask;
            fewest = count;
            if (count <= 1) break;
        }
        if (options) {
            const unsigned bit = first_bit(options);
            stack[depth].cell = cell;
            stack[depth].bit = bit;
            stack[depth].remaining = options ^ bit;
            ++depth;
            toggle_digit(&grid, cell, bit);
            continue;
        }
        if (cell < 0) {
            if (found) return false;
            found = true;
        }
        while (depth) {
            const int top = depth - 1;
            toggle_digit(&grid, stack[top].cell, stack[top].bit);
            if (stack[top].remaining) {
                const unsigned bit = first_bit(stack[top].remaining);
                stack[top].remaining ^= bit;
                stack[top].bit = bit;
                toggle_digit(&grid, stack[top].cell, bit);
                break;
            }
            --depth;
        }
        if (!depth) return found;
    }
}

static int unit_cell(int unit, int i) {
    if (unit < SIDE) return unit * SIDE + i;
    if (unit < 2 * SIDE) return i * SIDE + unit - SIDE;
    const int box = unit - 2 * SIDE;
    return (box / 3 * 3 + i / 3) * SIDE + box % 3 * 3 + i % 3;
}

/* Same difficulty rules as the built-in bank: naked singles, hidden singles, beyond singles. */
static unsigned grade(const char *digits) {
    grid_t grid;
    if (!read_grid(&grid, digits)) return SUDOKU_LEVELS;
    unsigned level = 0;
    for (;;) {
        uint16_t masks[CELLS] = {0};
        int single = -1;
        bool complete = true;
        for (int cell = 0; cell < CELLS; ++cell) {
            if (grid.digits[cell]) continue;
            complete = false;
            const unsigned mask = candidates(&grid, cell);
            masks[cell] = mask;
            if (__builtin_popcount(mask) == 1) {
                single = cell;
                break;
            }
        }
        if (complete) return level;
        if (single >= 0) {
            toggle_digit(&grid, single, masks[single]);
            continue;
        }
        level = 1;
        for (int unit = 0; unit < 3 * SIDE && single < 0; ++unit) {
            unsigned seen = 0, repeated = 0;
            for (int i = 0; i < SIDE; ++i) {
                const unsigned mask = masks[unit_cell(unit, i)];
                repeated |= seen & mask;
                seen |= mask;
            }
            const unsigned unique = seen & ~repeated;
            if (!unique) continue;
            const unsigned bit = first_bit(unique);
            for (int i = 0; i < SIDE; ++i) {
                const int cell = unit_cell(unit, i);
                if (!(masks[cell] & bit)) continue;
                toggle_digit(&grid, cell, bit);
                single = cell;
                break;
            }
        }
        if (single < 0) return 2;
    }
}

bool sudoku_generate(sudoku_puzzle_t *out, unsigned level, uint32_t seed) {
    static const uint8_t clues[SUDOKU_LEVELS] = {39, 33, 29};
    if (level >= SUDOKU_LEVELS) return false;
    if (!seed) seed = 1;
    sudoku_puzzle_t puzzle;
    permute_puzzle(&puzzle, NULL, &seed);
    uint8_t order[CELLS];
    shuffle(order, CELLS, &seed);
    unsigned remaining = CELLS, budget = SEARCH_BUDGET;
    for (int i = 0; i < CELLS && budget; ++i) {
        const int cell = order[i];
        puzzle.givens[cell] = '0';
        if (!has_unique_solution(puzzle.givens, &budget)) {
            puzzle.givens[cell] = puzzle.solution[cell];
            continue;
        }
        if (--remaining != clues[level]) continue;
        if (grade(puzzle.givens) != level) return false;
        *out = puzzle;
        return true;
    }
    return false;
}
