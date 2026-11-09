/* solitaire_core.c - Klondike solitaire game logic.
 * No platform includes, no drawing, no input. The front end owns
 * the screen and the keys; this file owns the rules and the state.
 */

#include "sol_core.h"

pile *piles[PILE_COUNT];
int   score;
unsigned long current_seed = 0;
card  deal_order[RANK_COUNT * SUIT_COUNT];
void (*shuffle_hook)(int done, int total) = NULL;

static undo_state undo_stack[UNDO_DEPTH];
static int undo_top = 0;

static const char pile_types[] = "dwffffccccccc";

/* ---------------- memory ----------------
 * Static pools instead of malloc/free: no heap setup needed on the
 * Spectrum, and no way to run out of memory at runtime.
 */
#define MAX_CARDS 64
#define MAX_NODES 64

static card      card_pool[MAX_CARDS];
static card_node node_pool[MAX_NODES];
static pile      pile_pool[PILE_COUNT];
static unsigned char card_used[MAX_CARDS];
static unsigned char node_used[MAX_NODES];
static unsigned char pile_used[PILE_COUNT];

static card *new_card(void)
{
    int i;
    for (i = 0; i < MAX_CARDS; i++)
        if (!card_used[i]) {
            card_used[i] = 1;
            memset(&card_pool[i], 0, sizeof(card));
            return &card_pool[i];
        }
    return NULL;
}
static void free_card(card *c)
{
    int i = (int)(c - card_pool);
    if (c && i >= 0 && i < MAX_CARDS) card_used[i] = 0;
}
static card_node *new_node(void)
{
    int i;
    for (i = 0; i < MAX_NODES; i++)
        if (!node_used[i]) {
            node_used[i] = 1;
            memset(&node_pool[i], 0, sizeof(card_node));
            return &node_pool[i];
        }
    return NULL;
}
static void free_node(card_node *n)
{
    int i = (int)(n - node_pool);
    if (n && i >= 0 && i < MAX_NODES) node_used[i] = 0;
}
static pile *new_pile(void)
{
    int i;
    for (i = 0; i < PILE_COUNT; i++)
        if (!pile_used[i]) {
            pile_used[i] = 1;
            memset(&pile_pool[i], 0, sizeof(pile));
            return &pile_pool[i];
        }
    return NULL;
}
static void free_pile(pile *p)
{
    int i = (int)(p - pile_pool);
    if (p && i >= 0 && i < PILE_COUNT) pile_used[i] = 0;
}

/* ---------------- suit / rank ---------------- */
const char *suit_ch(int suit)
{
    switch (suit) {
    case SUIT_HEART:   return "H";
    case SUIT_SPADE:   return "S";
    case SUIT_CLUB:    return "C";
    case SUIT_DIAMOND: return "D";
    default:           return "?";
    }
}

const char *rank_ch(int rank)
{
    static const char *r[] = { "A","2","3","4","5","6","7","8","9","T",
                               "J","Q","K" };
    if (rank < 0 || rank >= RANK_COUNT) return "?";
    return r[rank];
}

int is_red_card(const card *c)
{
    return c->suit == SUIT_HEART || c->suit == SUIT_DIAMOND;
}
int is_black_card(const card *c)
{
    return c->suit == SUIT_CLUB || c->suit == SUIT_SPADE;
}
static int is_alternate(const card *a, const card *b)
{
    return is_black_card(a) != is_black_card(b);
}
static int is_in_sequence(const card *lower, const card *higher)
{
    return higher->rank == lower->rank + 1;
}
static int can_place_on_column(const card *parent, const card *child)
{
    return is_alternate(parent, child) && is_in_sequence(child, parent);
}
static int can_place_on_foundation(const card *parent, const card *child)
{
    return parent->suit == child->suit && is_in_sequence(parent, child);
}

/* ---------------- slots ---------------- */
int slot_to_pile(int slot)
{
    if (slot == SLOT_STOCK) return P_DECK;
    if (slot == SLOT_WASTE) return P_WASTE;
    if (slot >= SLOT_FDN1 && slot < SLOT_FDN1 + FDN_COUNT)
        return P_FDN1 + (slot - SLOT_FDN1);
    if (slot >= SLOT_COL1 && slot < SLOT_COL1 + COL_COUNT)
        return P_COL1 + (slot - SLOT_COL1);
    return -1;
}

int slot_x(int slot)
{
    if (slot == SLOT_STOCK) return 0;
    if (slot == SLOT_WASTE) return COL_SP;
    if (slot >= SLOT_FDN1 && slot < SLOT_FDN1 + FDN_COUNT)
        return (3 + (slot - SLOT_FDN1)) * COL_SP;
    if (slot >= SLOT_COL1 && slot < SLOT_COL1 + COL_COUNT)
        return (slot - SLOT_COL1) * COL_SP;
    return 0;
}

/* ---------------- pile primitives ---------------- */
static card_node *make_node(card *c)
{
    card_node *n = new_node();
    if (n) n->value = c;
    return n;
}
static int is_empty(pile *p) { return p->num_cards == 0; }

static void push(pile *p, card *c)
{
    card_node *t = p->head;
    if (!t) p->head = make_node(c);
    else {
        while (t->next) t = t->next;
        t->next = make_node(c);
    }
    p->num_cards++;
}

static card *shift(pile *p)
{
    card_node *h = p->head;
    card *c;
    if (!h) return NULL;
    p->head = h->next;
    c = h->value;
    free_node(h);
    p->num_cards--;
    return c;
}

card *peek_at(pile *p, int idx)
{
    card_node *h = p->head;
    int i;
    if (!p || !h) return NULL;
    for (i = 0; i < idx && h; i++) h = h->next;
    return h ? h->value : NULL;
}

card *peek_last(pile *p)
{
    if (!p || p->num_cards == 0) return NULL;
    return peek_at(p, p->num_cards - 1);
}

static void delete_card(pile *p, card *c)
{
    card_node *prev = NULL, *cur;
    if (is_empty(p)) return;
    for (cur = p->head; cur; prev = cur, cur = cur->next) {
        if (cur->value == c) {
            if (!prev) p->head = cur->next;
            else       prev->next = cur->next;
            p->num_cards--;
            free_node(cur);
            return;
        }
    }
}

static void insert_at(pile *p, card *c, int idx)
{
    card_node *cur = p->head, *n;
    int i;
    if (idx <= 0 || !cur) {
        n = make_node(c);
        n->next = p->head;
        p->head = n;
        p->num_cards++;
        return;
    }
    for (i = 0; i < idx - 1 && cur->next; i++) cur = cur->next;
    n = make_node(c);
    n->next = cur->next;
    cur->next = n;
    p->num_cards++;
}

static pile *make_pile(char type)
{
    pile *p = new_pile();
    if (p) p->type = type;
    return p;
}

/* ---------------- deck / deal / turn ---------------- */
static void fill_deck(pile *p)
{
    int r, s;
    for (r = 0; r < RANK_COUNT; r++)
        for (s = 0; s < SUIT_COUNT; s++) {
            card *c = new_card();
            if (!c) return;
            c->suit = (unsigned char)s;
            c->rank = (unsigned char)r;
            c->revealed = 0;
            push(p, c);
        }
}

static void shuffle_pile(pile *p)
{
    int n = p->num_cards * 10, i;
    for (i = 0; i < n; i++) {
        int idx;
        card *c;
        if (shuffle_hook && (i % 13) == 0) shuffle_hook(i, n);
        idx = (p->num_cards > 1) ? rand() % (p->num_cards - 1) : 0;
        c = shift(p);
        insert_at(p, c, idx);
    }
    if (shuffle_hook) shuffle_hook(n, n);   /* finish at 100% */
}

static void deal(void)
{
    int i, k;
    for (i = 0; i < COL_COUNT; i++) {
        pile *col = piles[P_COL1 + i];
        for (k = 0; k <= i; k++) {
            card *c = shift(piles[P_DECK]);
            if (c) {
                push(col, c);
                if (k == i) c->revealed = 1;
            }
        }
    }
    {
        card *c = shift(piles[P_DECK]);
        if (c) { c->revealed = 1; push(piles[P_WASTE], c); }
    }
}

static void turn(void)
{
    card *c = shift(piles[P_DECK]);
    if (!c) return;
    c->revealed = 1;
    push(piles[P_WASTE], c);
}

static void redeal(void)
{
    while (!is_empty(piles[P_WASTE])) {
        card *c = shift(piles[P_WASTE]);
        c->revealed = 0;
        push(piles[P_DECK], c);
    }
    score -= 100;
    if (score < 0) score = 0;
}

/* ---------------- scoring ---------------- */
static void add_score(int delta)
{
    score += delta;
    if (score < 0) score = 0;
}

/* ---------------- move logic ---------------- */
static int column_run_start(pile *sp)
{
    int i = sp->num_cards - 1;
    while (i > 0) {
        card *lower = peek_at(sp, i);
        card *higher = peek_at(sp, i - 1);
        if (!lower || !higher) break;
        if (!lower->revealed || !higher->revealed) break;
        if (!is_alternate(lower, higher)) break;
        if (!is_in_sequence(lower, higher)) break;
        i--;
    }
    return i;
}

static int pick_amount(pile *sp, pile *dp, int dst_is_fdn, int forced_index)
{
    if (sp->num_cards == 0) return 0;

    if (forced_index >= 0) {
        card *bottom;
        if (forced_index >= sp->num_cards) return 0;
        bottom = peek_at(sp, forced_index);
        if (!bottom || !bottom->revealed) return 0;
        if (dst_is_fdn) {
            if (forced_index != sp->num_cards - 1) return 0;
            {
                card *top = peek_last(dp);
                if (!top) return (bottom->rank == RANK_A) ? 1 : 0;
                return can_place_on_foundation(top, bottom) ? 1 : 0;
            }
        } else {
            card *dst_top = peek_last(dp);
            if (!dst_top)
                return (bottom->rank == RANK_K)
                     ? sp->num_cards - forced_index : 0;
            return can_place_on_column(dst_top, bottom)
                 ? sp->num_cards - forced_index : 0;
        }
    }

    if (dst_is_fdn) {
        card *top = peek_last(dp);
        card *sc  = peek_last(sp);
        if (!sc || !sc->revealed) return 0;
        if (!top) return (sc->rank == RANK_A) ? 1 : 0;
        return can_place_on_foundation(top, sc) ? 1 : 0;
    }
    {
        /* multi-card runs only exist in tableau columns; waste and
           foundation piles always move one card */
        int start = (sp->type == 'c') ? column_run_start(sp)
                                      : sp->num_cards - 1;
        int run_len = sp->num_cards - start;
        card *run_head = peek_at(sp, start);
        card *dst_top  = peek_last(dp);

        if (run_len > 0 && run_head && run_head->revealed) {
            if (!dst_top) {
                if (run_head->rank == RANK_K) return run_len;
            } else if (can_place_on_column(dst_top, run_head)) {
                return run_len;
            }
        }
        {
            card *top = peek_last(sp);
            if (top && top->revealed) {
                if (!dst_top) {
                    if (top->rank == RANK_K) return 1;
                } else if (can_place_on_column(dst_top, top)) {
                    return 1;
                }
            }
        }
        return 0;
    }
}

static void move_cards(pile *sp, pile *dp, int amount)
{
    int first_idx = sp->num_cards - amount;
    int i;
    for (i = 0; i < amount; i++) {
        card *c = peek_at(sp, first_idx);
        if (!c) break;
        delete_card(sp, c);
        push(dp, c);
    }
    {
        card *next = peek_last(sp);
        if (next && !next->revealed) { next->revealed = 1; add_score(5); }
    }
    if (sp->type == 'f' && dp->type == 'c') add_score(-15);   /* pull back penalty */
    if (dp->type == 'f') add_score(10);
    if (sp->type == 'w' && dp->type == 'c') add_score(5);
}

const char *try_move(int src_slot, int dst_slot, int src_index)
{
    int spi = slot_to_pile(src_slot);
    int dpi = slot_to_pile(dst_slot);
    pile *sp, *dp;
    int amount, dst_is_fdn;

    if (spi < 0 || dpi < 0) return "Invalid slot";

    if (dpi == P_DECK && spi != P_DECK) return "Cannot move to stock";

    sp = piles[spi];
    dp = piles[dpi];

    if (spi == P_DECK) {
        if (is_empty(piles[P_DECK])) {
            if (is_empty(piles[P_WASTE])) return "Cannot redeal";
            redeal();
            return "Redealt";
        }
        turn();
        return "Drew card";
    }

    if (is_empty(sp)) return "Source empty";
    dst_is_fdn = (dp->type == 'f');

    /* the waste pile is never a destination, and shuffling cards
       between foundations is pointless (and would farm score) */
    if (dp->type == 'w') return "Invalid move";
    if (dst_is_fdn && sp->type == 'f') return "Invalid move";

    amount = pick_amount(sp, dp, dst_is_fdn, src_index);
    if (amount == 0) return "Invalid move";

    move_cards(sp, dp, amount);
    return "OK";
}

int auto_foundation(void)
{
    int moved = 0;
    int progress = 1;

    while (progress) {
        int i;
        progress = 0;

        {
            card *c = peek_last(piles[P_WASTE]);
            if (c && c->revealed) {
                for (i = 0; i < FDN_COUNT; i++) {
                    pile *f = piles[P_FDN1 + i];
                    card *top = peek_last(f);
                    int ok = top ? can_place_on_foundation(top, c)
                                 : (c->rank == RANK_A);
                    if (ok) {
                        move_cards(piles[P_WASTE], f, 1);
                        moved++;
                        progress = 1;
                        break;
                    }
                }
            }
        }
        if (progress) continue;

        for (i = 0; i < COL_COUNT; i++) {
            pile *col = piles[P_COL1 + i];
            card *c = peek_last(col);
            int j;
            if (!c || !c->revealed) continue;
            for (j = 0; j < FDN_COUNT; j++) {
                pile *f = piles[P_FDN1 + j];
                card *top = peek_last(f);
                int ok = top ? can_place_on_foundation(top, c)
                             : (c->rank == RANK_A);
                if (ok) {
                    move_cards(col, f, 1);
                    moved++;
                    progress = 1;
                    break;
                }
            }
            if (progress) break;
        }
    }

    return moved;
}

int game_won(void)
{
    int i;
    for (i = 0; i < FDN_COUNT; i++)
        if (piles[P_FDN1 + i]->num_cards != 13) return 0;
    return 1;
}

/* ---------------- undo ---------------- */
static void capture_state(undo_state *u)
{
    int i;
    int out = 0;
    for (i = 0; i < PILE_COUNT; i++) {
        card_node *n = piles[i]->head;
        int pos = 0;
        while (n && out < (int)(sizeof(u->cards) / sizeof(u->cards[0]))) {
            u->cards[out].suit     = n->value->suit;
            u->cards[out].rank     = n->value->rank;
            u->cards[out].revealed = n->value->revealed;
            u->cards[out].pile     = (unsigned char)i;
            u->cards[out].pos      = (unsigned char)pos;
            out++;
            pos++;
            n = n->next;
        }
    }
    u->score = score;
    u->sel_row = 0;
    u->sel_col = 0;
    u->sel_depth = -1;
    u->selected = -1;
    u->selected_index = -1;
}

static void restore_state(const undo_state *u,
                          int *sel_row, int *sel_col, int *sel_depth,
                          int *selected, int *selected_index)
{
    int i, k;

    for (i = 0; i < PILE_COUNT; i++) {
        if (!piles[i])
            piles[i] = make_pile(pile_types[i]);
        {
            card_node *n = piles[i]->head;
            while (n) {
                card_node *nx = n->next;
                free_card(n->value);
                free_node(n);
                n = nx;
            }
        }
        piles[i]->head = NULL;
        piles[i]->num_cards = 0;
    }

    for (k = 0; k < (int)(sizeof(u->cards) / sizeof(u->cards[0])); k++) {
        int p = u->cards[k].pile;
        if (p >= 0 && p < PILE_COUNT && u->cards[k].suit < SUIT_COUNT) {
            card *c = new_card();
            if (!c) return;
            c->suit     = u->cards[k].suit;
            c->rank     = u->cards[k].rank;
            c->revealed = u->cards[k].revealed;
            push(piles[p], c);
        }
    }

    score = u->score;
    *sel_row = u->sel_row;
    *sel_col = u->sel_col;
    *sel_depth = u->sel_depth;
    *selected = u->selected;
    *selected_index = u->selected_index;
}

void undo_push(int sel_row, int sel_col, int sel_depth,
               int selected, int selected_index)
{
    undo_state *u;
    if (undo_top >= UNDO_DEPTH) {
        memmove(&undo_stack[0], &undo_stack[1],
                sizeof(undo_state) * (UNDO_DEPTH - 1));
        undo_top = UNDO_DEPTH - 1;
    }
    u = &undo_stack[undo_top];
    capture_state(u);
    u->sel_row = sel_row;
    u->sel_col = sel_col;
    u->sel_depth = sel_depth;
    u->selected = selected;
    u->selected_index = selected_index;
    undo_top++;
}

void undo_drop(void)
{
    if (undo_top > 0) undo_top--;
}

int undo_pop(int *sel_row, int *sel_col, int *sel_depth,
             int *selected, int *selected_index)
{
    if (undo_top <= 0) return 0;
    undo_top--;
    restore_state(&undo_stack[undo_top],
                  sel_row, sel_col, sel_depth, selected, selected_index);
    return 1;
}

/* ---------------- setup / cleanup ---------------- */
void prepare_game(unsigned long seed)
{
    int i;
    current_seed = seed;
    srand((unsigned)seed);

    for (i = 0; i < PILE_COUNT; i++)
        piles[i] = make_pile(pile_types[i]);
    fill_deck(piles[P_DECK]);
    shuffle_pile(piles[P_DECK]);

    {
        card_node *n = piles[P_DECK]->head;
        i = 0;
        while (n && i < (int)(sizeof(deal_order) / sizeof(deal_order[0]))) {
            deal_order[i++] = *n->value;
            n = n->next;
        }
    }
    deal();
    score = 0;
    undo_top = 0;
}

void replay_game(void)
{
    int i;
    for (i = 0; i < PILE_COUNT; i++)
        piles[i] = make_pile(pile_types[i]);

    for (i = 0; i < (int)(sizeof(deal_order) / sizeof(deal_order[0])); i++) {
        card *c = new_card();
        if (!c) break;
        *c = deal_order[i];
        c->revealed = 0;
        push(piles[P_DECK], c);
    }
    deal();
    score = 0;
    undo_top = 0;
}

void free_game(void)
{
    int i;
    for (i = 0; i < PILE_COUNT; i++) {
        if (!piles[i]) continue;
        {
            card_node *n = piles[i]->head;
            while (n) {
                card_node *nx = n->next;
                free_card(n->value);
                free_node(n);
                n = nx;
            }
        }
        free_pile(piles[i]);
        piles[i] = NULL;
    }
}