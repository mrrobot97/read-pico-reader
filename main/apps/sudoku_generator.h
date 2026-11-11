/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { SUDOKU_SIDE = 9, SUDOKU_CELLS = 81, SUDOKU_LEVELS = 3 };

typedef struct {
    char givens[SUDOKU_CELLS + 1];
    char solution[SUDOKU_CELLS + 1];
} sudoku_puzzle_t;

/* One bounded generation attempt. Only publishes a unique puzzle of the requested level. */
bool sudoku_generate(sudoku_puzzle_t *out, unsigned level, uint32_t seed);
/* A shuffled built-in puzzle keeps its unique solution and difficulty. */
void sudoku_shuffle_puzzle(sudoku_puzzle_t *out, const sudoku_puzzle_t *puzzle, uint32_t seed);
bool sudoku_puzzle_valid(const sudoku_puzzle_t *puzzle);
