/* solitaire_zx.c - ZX Spectrum Next front end for Klondike solitaire.
 *
 * Build:
 *   zcc +zxn -vn -O2 -DAMALLOC -o solitaire sol_core.c sol_zx.c -create-app -Cz"--clean --nex-load=yukon.nxi"
 */

#include <arch/zx.h>
#include <input.h>
#include <stdlib.h>
#include <string.h>
#include "sol_core.h"

/* ---- Layer 2 Background Setup ---- */
static void init_layer2(void)
{
    #asm
    /* Layer 2 resolution: 256x192 */
    ld bc, 0x243B
    ld a, 0x14
    out (c), a
    ld bc, 0x253B
    ld a, 0x01
    out (c), a

    /* Layer 2 priority: 0 (behind ULA text) */
    ld bc, 0x243B
    ld a, 0x15
    out (c), a
    ld bc, 0x253B
    ld a, 0x00
    out (c), a

    /* Enable Layer 2 */
    ld bc, 0x243B
    ld a, 0x6B
    out (c), a
    ld bc, 0x253B
    ld a, 0x01
    out (c), a
    #endasm
}

/* ---- direct screen driver ---- */
#define SCR_RAM   ((unsigned char *)0x4000)
#define ATTR_RAM  ((unsigned char *)0x5800)
#define ROM_FONT  ((const unsigned char *)0x3D00)   /* chars 32..127 */

/* background paper colour; ENTER cycles through the dark (non-bright)
 * Spectrum colours */
static const unsigned char bg_list[5] =
    { PAPER_BLACK, PAPER_BLUE, PAPER_RED, PAPER_MAGENTA, PAPER_GREEN };
static unsigned char bg_idx = 0;
static unsigned char bg = PAPER_BLACK;

/* marker ink that stays visible on the current background */
static unsigned char ink_on_bg(unsigned char ink)
{
    return (ink == (unsigned char)(bg >> 3)) ? INK_WHITE : ink;
}

/* attribute bytes: ink (bits 0-2) + paper (bits 3-5) + transparent (bit 7) */
#define ATTR_MASK 0xBF

static unsigned char cur_x, cur_y, cur_attr = 7;

/* back buffer = what we want; shadow = what is on screen now.
 * Drawing calls only touch the back buffer; flush() copies just the
 * cells that differ, so a cursor move redraws a handful of cells. */
#define CELLS 768
static unsigned char bk_ch[CELLS], bk_at[CELLS];
static unsigned char sh_ch[CELLS], sh_at[CELLS];

static void my_gotoxy(unsigned char x, unsigned char y)
{
    cur_x = (unsigned char)(x - 1);
    cur_y = (unsigned char)(y - 1);
}
static void my_setattr(unsigned char attr, unsigned char unused)
{
    (void)unused;
    cur_attr = (unsigned char)(attr & ATTR_MASK);
}
static void zx_printchar(char ch)
{
    unsigned idx;
    if (cur_x > 31 || cur_y > 23) return;
    idx = ((unsigned)cur_y << 5) + cur_x;
    bk_ch[idx] = (unsigned char)ch;
    bk_at[idx] = cur_attr;
    cur_x++;
}
static void my_printstr(const char *s) { while (*s) zx_printchar(*s++); }

/* clear the back buffer only */
static void cls_black(void)
{
    memset(bk_ch, ' ', CELLS);
    memset(bk_at, (INK_WHITE | bg | 0x80) & ATTR_MASK, CELLS);
}

/* clear the real screen and the shadow (call once at start) */
static void screen_init(void)
{
    //zx_cls(); /* Native Next clear screen (no arguments) */
    memset(SCR_RAM, 0, 6144);
    memset(ATTR_RAM, (INK_WHITE | bg | 0x80) & ATTR_MASK, CELLS);
    memset(sh_ch, ' ', CELLS);
    memset(sh_at, (INK_WHITE | bg | 0x80) & ATTR_MASK, CELLS);
    cls_black();
}

/* Custom glyphs, drawn by our own driver (no UDG RAM needed).
 * Character codes 0x90..0x94 map into this table. */
#define G_HEART   0x90
#define G_SPADE   0x91
#define G_CLUB    0x92
#define G_DIAMOND 0x93
#define G_TEN     0x94
#define G_LEFT    0x95   /* filled triangle, points left  */
#define G_DOWN    0x96   /* filled triangle, points down  */
#define G_RING    0x97   /* small circle                   */
#define G_BLOCK   0x98   /* solid block (progress bar)     */
static const unsigned char udg[9][8] = {
    { 0x00,0x66,0xFF,0xFF,0xFF,0x7E,0x3C,0x18 },   /* heart   */
    { 0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x18,0x3C },   /* spade   */
    { 0x18,0x3C,0x3C,0xDB,0xFF,0xDB,0x18,0x3C },   /* club    */
    { 0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x3C,0x18 },   /* diamond */
    { 0x46,0xC9,0x49,0x49,0x49,0x49,0xE6,0x00 },   /* "10" in one cell */
    { 0x00,0x06,0x1E,0x7E,0x7E,0x1E,0x06,0x00 },   /* left triangle   */
    { 0x00,0x7E,0x7E,0x3C,0x3C,0x18,0x18,0x00 },   /* down triangle   */
    { 0x00,0x3C,0x42,0x42,0x42,0x42,0x3C,0x00 },   /* small circle    */
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF }    /* solid block     */
};

static void draw_cell(unsigned char x, unsigned char y, unsigned idx)
{
    unsigned char c = bk_ch[idx];
    unsigned char *dst = SCR_RAM + ((unsigned)(y & 0xC0) << 5)
                       + ((unsigned)(y & 0x07) << 8)
                       + ((unsigned)(y & 0x38) << 2) + x;
    const unsigned char *g = 0;
    unsigned char i;

    if (c >= 32 && c < 128)               g = ROM_FONT + ((unsigned)(c - 32) << 3);
    else if (c >= G_HEART && c <= G_BLOCK)  g = udg[c - G_HEART];
    for (i = 0; i < 8; i++) {
        unsigned char b;
        if (g)             b = g[i];
        else if (c >= 128) b = (i & 1) ? 0xAA : 0x55;   /* card back */
        else               b = 0;
        *dst = b;
        dst += 256;
    }
    ATTR_RAM[idx] = bk_at[idx];
}

static void flush(void)
{
    unsigned idx = 0;
    unsigned char x, y;
    for (y = 0; y < 24; y++, idx += 32) {
        unsigned r = (unsigned)y << 5;
        if (!memcmp(bk_ch + r, sh_ch + r, 32) &&
            !memcmp(bk_at + r, sh_at + r, 32))
            continue;                       /* whole row unchanged */
        for (x = 0; x < 32; x++)
            if (bk_ch[r + x] != sh_ch[r + x] ||
                bk_at[r + x] != sh_at[r + x]) {
                if (bk_ch[r + x] == sh_ch[r + x])
                    ATTR_RAM[r + x] = bk_at[r + x];   /* colour only */
                else
                    draw_cell(x, y, r + x);
                sh_ch[r + x] = bk_ch[r + x];
                sh_at[r + x] = bk_at[r + x];
            }
    }
}

/* classic lib spells it in_Inkey; wait for key release so a held key
 * doesn't repeat at frame speed */
static int in_inkey(void) { return in_Inkey(); }
static void wait_release(void) { while (in_Inkey() != 0) ; }

/* Random seed 1..65535 from the ROM FRAMES counter (ticks 50x/sec),
 * mixed with a count of idle loops in case interrupts are off.
 * The core's srand() takes a 16-bit value, so seeds are 16-bit. */
static unsigned long random_seed(unsigned idle)
{
    volatile unsigned char *fr = (volatile unsigned char *)23672;
    unsigned v = (unsigned)fr[0] | ((unsigned)fr[1] << 8);
    v ^= idle * 31u;
    v = v * 25173u + 13849u;          /* scramble: spread small values */
    return v ? (unsigned long)v : 1UL;
}

static unsigned long chosen_seed = 1;

/* ---- screen geometry (32 x 24) ---- */
#define SCR_W        32
#define TOP_ROW       0
#define COL_ROW       2
#define STATUS_ROW   22
#define MSG_ROW      23

/* ---- keys ---- */
enum { KEY_NONE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
       KEY_SPACE, KEY_NEW, KEY_UNDO, KEY_AUTO, KEY_BG };

/* selection state (the core's run_solitaire used to own this) */
static int sel_row, sel_col, sel_depth;
static int selected, selected_index;
static char msg[64];   /* big enough for any strcpy below */

/* ---- tiny number printer (no snprintf) ---- */
static void utoa10(unsigned long v, char *out)
{
    char tmp[12];
    int i = 0, j = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
}

/* ---- one card at (x, y) ---- */
static void print_card(int x, int y, card *c)
{
    my_gotoxy((unsigned char)(x + 1), (unsigned char)(y + 1));

    if (!c) {
        my_setattr(INK_BLACK | PAPER_WHITE | 0x80, 1);
        my_printstr("[]");
        return;
    }
    if (!c->revealed) {
        /* face-down: cyan ink on blue paper, two block chars */
        my_setattr(INK_CYAN | PAPER_BLUE | 0x80, 1);
        my_printstr("\x8A\x8A");
        return;
    }
    my_setattr((is_red_card(c) ? INK_RED : INK_BLACK) | PAPER_WHITE | 0x80, 1);
    if (c->rank == RANK_10) zx_printchar((char)G_TEN);
    else                    my_printstr(rank_ch(c->rank));
    switch (c->suit) {
    case SUIT_HEART:   zx_printchar((char)G_HEART);   break;
    case SUIT_SPADE:   zx_printchar((char)G_SPADE);   break;
    case SUIT_CLUB:    zx_printchar((char)G_CLUB);    break;
    default:           zx_printchar((char)G_DIAMOND); break;
    }
}

/* ---- cursor / selection marker ----
 * Top row and individual cards: left triangle just right of the card.
 * Whole-column position (sel_depth == -1): down triangle on the blank row above
 * the column, so it is a visibly separate step from the first card. */
static void draw_marker(void)
{
    int cx, cy;
    char cg = (char)G_LEFT;

    if (sel_row == 0) {
        cx = slot_x(sel_col) + 2;
        cy = TOP_ROW;
    } else if (sel_depth < 0) {
        cx = sel_col * COL_SP;
        cy = COL_ROW - 1;
        cg = (char)G_DOWN;
    } else {
        cx = sel_col * COL_SP + 2;
        cy = COL_ROW + sel_depth;
    }
    if (cx >= SCR_W) cx = SCR_W - 1;

    my_gotoxy((unsigned char)(cx + 1), (unsigned char)(cy + 1));
    my_setattr(ink_on_bg(INK_YELLOW) | bg | 0x80, 1);
    zx_printchar(cg);

    /* if something is selected, mark that cell too */
    if (selected >= 0) {
        int sx, sy;
        char sg = (char)G_LEFT;
        if (selected >= SLOT_COL1) {
            int cidx = selected - SLOT_COL1;
            if (selected_index >= 0) {
                sx = cidx * COL_SP + 2;
                sy = COL_ROW + selected_index;
            } else {
                sx = cidx * COL_SP;
                sy = COL_ROW - 1;
                sg = (char)G_DOWN;
            }
        } else {
            sx = slot_x(selected) + 2;
            sy = TOP_ROW;
        }
        if (sx >= SCR_W) sx = SCR_W - 1;
        my_gotoxy((unsigned char)(sx + 1), (unsigned char)(sy + 1));
        if (sx == cx && sy == cy) {
            my_setattr(ink_on_bg(INK_MAGENTA) | bg | 0x80, 1);
            zx_printchar((char)G_RING);
        } else {
            my_setattr(ink_on_bg(INK_RED) | bg | 0x80, 1);
            zx_printchar(sg);
        }
    }
}

/* ---- status line ---- */
static void print_status(void)
{
    char num[12];
    my_gotoxy(1, STATUS_ROW + 1);
    my_setattr(INK_WHITE | bg | 0x80, 1);
    my_printstr("Seed #");
    utoa10(current_seed, num);
    my_printstr(num);
}

/* ---- message line ----
 * Message on the left; the score ("S:340") is right-aligned on the same
 * row whenever there is room for it. */
static void print_message(const char *s)
{
    char sc[14];
    int n = 0, len = 0, sl;

    utoa10((unsigned long)(score < 0 ? 0 : score), sc + 2);
    sc[0] = 'S'; sc[1] = ':';
    sl = (int)strlen(sc);

    my_gotoxy(1, MSG_ROW + 1);
    my_setattr(INK_WHITE | bg | 0x80, 1);
    if (s) len = (int)strlen(s);
    if (len > SCR_W) len = SCR_W;

    while (n < len) { zx_printchar(s[n]); n++; }
    while (n < SCR_W) { zx_printchar(' '); n++; }

    if (len + 1 + sl <= SCR_W) {            /* room for the score */
        my_gotoxy((unsigned char)(SCR_W - sl + 1), MSG_ROW + 1);
        my_setattr(INK_YELLOW | bg | 0x80, 1);
        my_printstr(sc);
    }
}

/* ---- full repaint ---- */
static void render(void)
{
    int i, r;

    cls_black();

    print_card(slot_x(SLOT_STOCK), TOP_ROW, peek_last(piles[P_DECK]));
    print_card(slot_x(SLOT_WASTE), TOP_ROW, peek_last(piles[P_WASTE]));
    for (i = 0; i < FDN_COUNT; i++)
        print_card(slot_x(SLOT_FDN1 + i), TOP_ROW,
                   peek_last(piles[P_FDN1 + i]));

    for (i = 0; i < COL_COUNT; i++) {
        pile *p = piles[P_COL1 + i];
        for (r = 0; r < p->num_cards && r < MAX_STACK; r++)
            print_card(i * COL_SP, COL_ROW + r, peek_at(p, r));
        if (p->num_cards == 0)
            print_card(i * COL_SP, COL_ROW, NULL);
    }

    draw_marker();
    print_status();
    print_message(msg);
    flush();
}

/* ---- input ---- */
static int read_key(void)
{
    int k;
    wait_release();            /* previous key must be up first */
    for (;;) {
        k = in_inkey();
        if (k != 0) break;
    }
    switch (k) {
    case 'q': case 'Q': return KEY_UP;
    case 'a': case 'A': return KEY_DOWN;
    case 'o': case 'O': return KEY_LEFT;
    case 'p': case 'P': return KEY_RIGHT;
    case ' ':           return KEY_SPACE;
    case 'n': case 'N': return KEY_NEW;
    case 'z': case 'Z': return KEY_UNDO;
    case 'f': case 'F': return KEY_AUTO;
    case 13:            return KEY_BG;       /* ENTER */
    }
    return KEY_NONE;
}

/* ---- cursor movement (ported from run_solitaire) ---- */
static void move_cursor(int key)
{
    static const int to_top[7] = {0, 1, 2, 2, 3, 4, 5};
    static const int to_col[6] = {0, 1, 2, 3, 4, 5};

    if (key == KEY_UP) {
        if (sel_row == 1 && sel_depth > 0) sel_depth--;
        else if (sel_row == 1 && sel_depth == 0) sel_depth = -1;
        else if (sel_row == 1) {
            sel_row = 0; sel_col = to_top[sel_col]; sel_depth = -1;
        }
    } else if (key == KEY_DOWN) {
        if (sel_row == 0) {
            sel_row = 1; sel_col = to_col[sel_col]; sel_depth = -1;
        } else if (sel_depth < 0) {
            pile *p = piles[P_COL1 + sel_col];
            if (p->num_cards > 0) sel_depth = p->num_cards - 1;
        } else {
            pile *p = piles[P_COL1 + sel_col];
            if (sel_depth < p->num_cards - 1) sel_depth++;
            else sel_depth = -1;
        }
    } else if (key == KEY_LEFT) {
        if (sel_row == 0) {
            sel_col = (sel_col > 0) ? sel_col - 1 : 5;
        } else {
            if (sel_col > 0) sel_col--; else sel_col = COL_COUNT - 1;
            {
                pile *p = piles[P_COL1 + sel_col];
                if (sel_depth >= p->num_cards)
                    sel_depth = (p->num_cards > 0) ? p->num_cards - 1 : -1;
            }
        }
    } else if (key == KEY_RIGHT) {
        if (sel_row == 0) {
            sel_col = (sel_col < 5) ? sel_col + 1 : 0;
        } else {
            if (sel_col < COL_COUNT - 1) sel_col++; else sel_col = 0;
            {
                pile *p = piles[P_COL1 + sel_col];
                if (sel_depth >= p->num_cards)
                    sel_depth = (p->num_cards > 0) ? p->num_cards - 1 : -1;
            }
        }
    }
}

/* ---- new-game prompt ----
 * Type up to 5 digits (0..65535) then ENTER to deal that seed;
 * ENTER on its own deals a random new game.
 * R = replay the current deal, Esc/CAPS+1 = cancel.
 * Returns 0 cancel, 1 new game (seed in chosen_seed), 2 replay. */
static int prompt_new_game(void)
{
    char buf[8];
    int len = 0;
    unsigned idle = 0;
    buf[0] = 0;

    for (;;) {
        int k;
        strcpy(msg, "Seed (ENTER=new, R=redo): ");
        strcat(msg, buf);
        render();
        wait_release();
        for (;;) { k = in_inkey(); if (k) break; idle++; }

        if (k == 13) {                               /* ENTER */
            if (len == 0) {
                chosen_seed = random_seed(idle);
            } else {
                unsigned long v = 0;
                int i;
                for (i = 0; i < len; i++) v = v * 10 + (unsigned long)(buf[i] - '0');
                chosen_seed = v;
            }
            return 1;
        }
        if (k == 'r' || k == 'R') return 2;
        if (k == 1 || k == 7) return 0;              /* Esc / CAPS+1 */
        if ((k == 12 || k == 8) && len > 0) {        /* DELETE */
            buf[--len] = 0;
        } else if (k >= '0' && k <= '9' && len < 5) {
            unsigned long v = 0;
            int i;
            buf[len] = (char)k; buf[len + 1] = 0;
            for (i = 0; i <= len; i++) v = v * 10 + (unsigned long)(buf[i] - '0');
            if (v > 65535UL) buf[len] = 0;           /* too big: reject digit */
            else len++;
        }
    }
}

/* ---- shuffle progress bar: "Shuffling [########............]" ----
 * Drawn on row bar_row (0-based). The core calls this while shuffling. */
static int bar_row = MSG_ROW;

static void shuffle_progress(int done, int total)
{
    int i, filled = (total > 0) ? (int)(((long)done * 20) / total) : 0;
    my_gotoxy(1, (unsigned char)(bar_row + 1));
    my_setattr(ink_on_bg(INK_YELLOW) | bg | 0x80, 1);
    my_printstr("Shuffling [");
    for (i = 0; i < 20; i++)
        zx_printchar(i < filled ? (char)G_BLOCK : '.');
    zx_printchar(']');
    flush();
}

/* ---- title / instructions screen ---- */
static void centre(int row, const char *s)
{
    int len = (int)strlen(s);
    my_gotoxy((unsigned char)((SCR_W - len) / 2 + 1), (unsigned char)(row + 1));
    my_printstr(s);
}
static void left(int row, const char *s)
{
    my_gotoxy(3, (unsigned char)(row + 1));
    my_printstr(s);
}

static void show_title(void)
{
    cls_black();
    my_setattr(ink_on_bg(INK_YELLOW) | bg | 0x80, 1);
    centre(1, "\x90\x91  SOLITAIRE  \x92\x93");

    my_setattr(INK_WHITE | bg | 0x80, 1);
    centre(3, "- KEYS -");
    left(5,  "Q / A   cursor up / down");
    left(6,  "O / P   cursor left / right");
    left(7,  "SPACE   pick up, then put down");
    left(8,  "        stock: draw a card");
    left(9,  "F       auto to foundations");
    left(10, "Z       undo (up to 32 moves)");
    left(11, "N       new / replay / seed");
    left(12, "ENTER   change background");

    my_setattr(ink_on_bg(INK_YELLOW) | bg | 0x80, 1);
    my_gotoxy(3, 16);
    zx_printchar((char)G_LEFT);  my_printstr(" card  ");
    zx_printchar((char)G_DOWN);  my_printstr(" column  ");
    zx_printchar((char)G_RING);  my_printstr(" chosen");

    my_setattr(INK_WHITE | bg | 0x80, 1);
    centre(17, "Build A to K by suit up top.");
    centre(18, "Stack down, alternate colours.");

    centre(21, "Press any key to deal");
    flush();
}

/* Hold the loading screen for about 3 seconds (any key skips it).
 * Uses the ROM FRAMES counter (50 ticks/sec); the iteration cap stops
 * it hanging forever if interrupts happen to be disabled. */
static void pause_for_loading_screen(void)
{
    volatile unsigned char *fr = (volatile unsigned char *)23672;
    unsigned char start = fr[0];
    unsigned guard = 0;

    while ((unsigned char)(fr[0] - start) < 150 && guard < 30000u) {
        if (in_inkey() != 0) break;
        guard++;
    }
    wait_release();
}

/* ---- main loop ---- */
int main(void)
{
    init_layer2();                  /* Enable the Layer 2 background */

    pause_for_loading_screen();     /* let the .scr show first */
    zx_border(INK_BLACK);
    screen_init();

    shuffle_hook = shuffle_progress;
    show_title();
    {
        unsigned idle = 0;
        while (in_inkey() == 0) idle++;
        bar_row = 19;                      /* progress bar under the text */
        prepare_game(random_seed(idle));
    }

    sel_row = 0; sel_col = 0; sel_depth = -1;
    selected = -1; selected_index = -1;
    strcpy(msg, "QAOP SPC sel F auto N new Z undo");

    render();

    for (;;) {
        int key = read_key();

        if (key == KEY_LEFT || key == KEY_RIGHT ||
            key == KEY_UP   || key == KEY_DOWN) {
            move_cursor(key);
            render();
            continue;
        }

        if (key == KEY_UNDO) {
            if (!undo_pop(&sel_row, &sel_col, &sel_depth,
                          &selected, &selected_index))
                strcpy(msg, "Nothing to undo.");
            else
                strcpy(msg, "Undone.");
            render();
            continue;
        }

        if (key == KEY_NEW) {
            int answer = prompt_new_game();
            if (answer) {
                free_game();
                bar_row = MSG_ROW;
                if (answer == 1) prepare_game(chosen_seed);
                else             replay_game();
                sel_row = 0; sel_col = 0; sel_depth = -1;
                selected = -1; selected_index = -1;
                strcpy(msg, answer == 1 ? "New game." : "Replay.");
                render();
            } else {
                strcpy(msg, "QAOP SPC sel F auto N new Z undo");
                render();
            }
            continue;
        }

        if (key == KEY_BG) {
            bg_idx = (unsigned char)((bg_idx + 1) % 5);
            bg = bg_list[bg_idx];
            render();
            continue;
        }

        if (key == KEY_AUTO) {
            int n;
            undo_push(sel_row, sel_col, sel_depth, selected, selected_index);
            n = auto_foundation();
            selected = -1; selected_index = -1;
            if (n == 0) {
                undo_drop();
                strcpy(msg, "Nothing to move.");
            } else {
                strcpy(msg, "Auto-moved.");
                /* column sizes changed: keep the cursor on a real card */
                if (sel_row == 1) {
                    pile *p = piles[P_COL1 + sel_col];
                    if (sel_depth >= p->num_cards)
                        sel_depth = (p->num_cards > 0) ? p->num_cards - 1 : -1;
                }
            }
            if (game_won())
                strcpy(msg, "YOU WIN!  N = new game");
            render();
            continue;
        }

        if (key == KEY_SPACE) {
            int slot = (sel_row == 0) ? sel_col : (SLOT_COL1 + sel_col);

            if (selected < 0) {
                if (slot == SLOT_STOCK) {
                    undo_push(sel_row, sel_col, sel_depth,
                              selected, selected_index);
                    {
                        const char *r0 = try_move(SLOT_STOCK, SLOT_STOCK, -1);
                        strcpy(msg, r0);
                        if (r0[0] != 'R' && r0[0] != 'D')
                            undo_drop();          /* nothing changed */
                    }
                } else {
                    selected = slot;
                    selected_index = (sel_depth >= 0) ? sel_depth : -1;
                    strcpy(msg, "Selected. Pick dest.");
                }
            } else if (selected == slot &&
                       selected_index == ((sel_depth >= 0) ? sel_depth : -1)) {
                selected = -1; selected_index = -1;
                strcpy(msg, "Cancelled.");
            } else {
                const char *r;
                undo_push(sel_row, sel_col, sel_depth,
                          selected, selected_index);
                r = try_move(selected, slot, selected_index);
                strcpy(msg, r);
                if (r[0] == 'O' || r[0] == 'R' || r[0] == 'D') {
                    if (slot >= SLOT_COL1) {
                        sel_row = 1;
                        sel_col = slot - SLOT_COL1;
                        sel_depth = -1;
                    } else {
                        sel_row = 0;
                        sel_col = slot;
                        sel_depth = -1;
                    }
                    selected = -1; selected_index = -1;
                }
                else
                    undo_drop();          /* failed move: nothing changed */
            }
            if (game_won())
                strcpy(msg, "YOU WIN!  N = new game");
            render();
            continue;
        }
    }
}