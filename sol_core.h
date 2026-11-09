/* solitaire_core.h - Klondike solitaire game logic, no platform deps. */
#ifndef SOLITAIRE_CORE_H
#define SOLITAIRE_CORE_H

#include <stdlib.h>
#include <string.h>

/* ---------------- geometry ---------------- */
#define COL_SP     4        /* 7 cols * 4 = 28, fits 32-char screen */
#define COL_COUNT  7
#define FDN_COUNT  4
#define MAX_STACK 19        /* fits 24-row screen */

/* ---------------- cards ---------------- */
enum { SUIT_HEART, SUIT_SPADE, SUIT_CLUB, SUIT_DIAMOND, SUIT_COUNT };
enum { RANK_A, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7,
       RANK_8, RANK_9, RANK_10, RANK_J, RANK_Q, RANK_K, RANK_COUNT };

typedef struct card {
    unsigned char suit, rank, revealed;
} card;

enum {
    P_DECK, P_WASTE,
    P_FDN1, P_FDN2, P_FDN3, P_FDN4,
    P_COL1, P_COL2, P_COL3, P_COL4, P_COL5, P_COL6, P_COL7,
    PILE_COUNT
};

typedef struct card_node {
    card *value;
    struct card_node *next;
} card_node;

typedef struct pile {
    card_node *head;
    int num_cards;
    char type;
} pile;

/* ---------------- slots ---------------- */
#define SLOT_STOCK   0
#define SLOT_WASTE   1
#define SLOT_FDN1    2
#define SLOT_COL1    6
#define SLOT_COUNT   13

/* ---------------- undo ---------------- */
#define UNDO_DEPTH 32   /* ~275 bytes per level */

typedef struct {
    unsigned char suit;
    unsigned char rank;
    unsigned char revealed;
    unsigned char pile;
    unsigned char pos;
} undo_card;

typedef struct {
    undo_card cards[RANK_COUNT * SUIT_COUNT];
    int score;
    int sel_row, sel_col, sel_depth;
    int selected, selected_index;
} undo_state;

/* ---------------- shared state ---------------- */
extern pile *piles[PILE_COUNT];
extern int   score;
extern unsigned long current_seed;
extern card  deal_order[RANK_COUNT * SUIT_COUNT];
/* optional: called now and then while shuffling (done, total) */
extern void (*shuffle_hook)(int done, int total);

/* ---------------- prototypes ---------------- */
int  slot_to_pile(int slot);
int  slot_x(int slot);
const char *suit_ch(int suit);
const char *rank_ch(int rank);
int  is_red_card(const card *c);
int  is_black_card(const card *c);

card *peek_at(pile *p, int idx);
card *peek_last(pile *p);

void prepare_game(unsigned long seed);
void replay_game(void);
void free_game(void);

const char *try_move(int src_slot, int dst_slot, int src_index);
int  auto_foundation(void);
int  game_won(void);

void undo_push(int sel_row, int sel_col, int sel_depth,
               int selected, int selected_index);
int  undo_pop(int *sel_row, int *sel_col, int *sel_depth,
              int *selected, int *selected_index);
void undo_drop(void);   /* discard newest snapshot without restoring it */

#endif