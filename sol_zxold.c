/* solitaire_zx.c - ZX Spectrum front end for Klondike solitaire.
 *
 * Build (classic z88dk lib, no heap needed):
 *   zcc +zx -vn -O2 -o solitaire sol_core.c sol_zx.c -create-app
 *
 * Keys: Q=up A=down O=left P=right  Space=select/move/cancel
 *       N=new game  Z=undo
 *       Esc (CAPS SHIFT+1) = quit
 */

#include <arch/zx.h>
#include <input.h>
#include <stdlib.h>
#include <string.h>
#include "sol_core.h"

/* ---- direct screen driver ----
 * Draws characters straight into screen RAM using the ROM font, so it
 * does not depend on the z88dk console (no AT codes, no scrolling).
 * x,y passed to zx_gotoxy are 1-based, matching the calls below.
 * Attribute bytes are INK_x | PAPER_y as defined in <arch/zx.h>. */
#define SCR_RAM   ((unsigned char *)0x4000)
#define ATTR_RAM  ((unsigned char *)0x5800)
#define ROM_FONT  ((const unsigned char *)0x3D00)   /* chars 32..127 */

static unsigned char cur_x, cur_y, cur_attr = 7;

/* back buffer = what we want; shadow = what is on screen now.
 * Drawing calls only touch the back buffer; flush() copies just the
 * cells that differ, so a cursor move redraws a handful of cells. */
#define CELLS 768
static unsigned char bk_ch[CELLS], bk_at[CELLS];
static unsigned char sh_ch[CELLS], sh_at[CELLS];

static void zx_gotoxy(unsigned char x, unsigned char y)
{
    cur_x = (unsigned char)(x - 1);
    cur_y = (unsigned char)(y - 1);
}
static void zx_setattr(unsigned char attr, unsigned char unused)
{
    (void)unused;
    cur_attr = attr;
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
static void zx_printstr(const char *s) { while (*s) zx_printchar(*s++); }

/* clear the back buffer only */
static void cls_black(void)
{
    memset(bk_ch, ' ', CELLS);
    memset(bk_at, INK_WHITE | PAPER_BLACK, CELLS);
}

/* clear the real screen and the shadow (call once at start) */
static void screen_init(void)
{
    memset(SCR_RAM, 0, 6144);
    memset(ATTR_RAM, INK_WHITE | PAPER_BLACK, CELLS);
    memset(sh_ch, ' ', CELLS);
    memset(sh_at, INK_WHITE | PAPER_BLACK, CELLS);
    cls_black();
}

static void draw_cell(unsigned char x, unsigned char y, unsigned idx)
{
    unsigned char c = bk_ch[idx];
    unsigned char *dst = SCR_RAM + ((unsigned)(y & 24) << 8)
                       + ((unsigned)(y & 7) << 5) + x;
    const unsigned char *g = 0;
    unsigned char i;

    if (c >= 32 && c < 128) g = ROM_FONT + ((unsigned)(c - 32) << 3);
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
    for (y = 0; y < 24; y++)
        for (x = 0; x < 32; x++, idx++)
            if (bk_ch[idx] != sh_ch[idx] || bk_at[idx] != sh_at[idx]) {
                draw_cell(x, y, idx);
                sh_ch[idx] = bk_ch[idx];
                sh_at[idx] = bk_at[idx];
            }
}

/* classic lib spells it in_Inkey; wait for key release so a held key
 * doesn't repeat at frame speed */
static int in_inkey(void) { return in_Inkey(); }
static void wait_release(void) { while (in_Inkey() != 0) ; }

static unsigned long make_seed(void)
{
    unsigned long s;
    while ((s = (unsigned long)in_inkey()) == 0) ;
    return s ? s : 1;
}
/* ---- screen geometry (32 x 24) ---- */
#define SCR_W        32
#define TOP_ROW       0
#define COL_ROW       2
#define STATUS_ROW   22
#define MSG_ROW      23

/* ---- keys ---- */
enum { KEY_NONE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
       KEY_SPACE, KEY_NEW, KEY_UNDO, KEY_AUTO, KEY_QUIT };

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
    zx_gotoxy((unsigned char)(x + 1), (unsigned char)(y + 1));

    if (!c) {
        zx_setattr(INK_BLACK | PAPER_WHITE, 1);
        zx_printstr("[]");
        return;
    }
    if (!c->revealed) {
        /* face-down: cyan ink on blue paper, two block chars */
        zx_setattr(INK_CYAN | PAPER_BLUE, 1);
        zx_printstr("\x8A\x8A");
        return;
    }
    zx_setattr((is_red_card(c) ? INK_RED : INK_BLACK) | PAPER_WHITE, 1);
    zx_printstr(rank_ch(c->rank));
    zx_printstr(suit_ch(c->suit));
}

/* ---- cursor / selection marker ---- */
static void draw_marker(void)
{
    int cx, cy;

    if (sel_row == 0) {
        cx = slot_x(sel_col) + 2;
        cy = TOP_ROW;
    } else {
        cx = sel_col * COL_SP + 2;
        cy = COL_ROW + ((sel_depth >= 0) ? sel_depth : 0);
    }
    if (cx >= SCR_W) cx = SCR_W - 1;

    zx_gotoxy((unsigned char)(cx + 1), (unsigned char)(cy + 1));
    zx_setattr(INK_YELLOW | PAPER_BLACK, 1);
    zx_printstr("<");

    /* if something is selected, mark that cell too */
    if (selected >= 0) {
        int sx, sy;
        if (selected >= SLOT_COL1) {
            int cidx = selected - SLOT_COL1;
            sx = cidx * COL_SP + 2;
            sy = COL_ROW + ((selected_index >= 0) ? selected_index : 0);
        } else {
            sx = slot_x(selected) + 2;
            sy = TOP_ROW;
        }
        if (sx >= SCR_W) sx = SCR_W - 1;
        zx_gotoxy((unsigned char)(sx + 1), (unsigned char)(sy + 1));
        zx_setattr((sx == cx && sy == cy) ? (INK_MAGENTA | PAPER_BLACK)
                                          : (INK_RED     | PAPER_BLACK), 1);
        zx_printstr((sx == cx && sy == cy) ? "+" : "<");
    }
}

/* ---- status line ---- */
static void print_status(void)
{
    char num[12];
    zx_gotoxy(1, STATUS_ROW + 1);
    zx_setattr(INK_WHITE | PAPER_BLACK, 1);
    zx_printstr("S:");
    utoa10((unsigned long)(score < 0 ? 0 : score), num);
    zx_printstr(num);
    zx_printstr("  #");
    utoa10(current_seed, num);
    zx_printstr(num);

    /* blank the rest of the row */
    {
        int used = 2 + (int)strlen(num);
        (void)used;
    }
}

/* ---- message line ---- */
static void print_message(const char *s)
{
    int n = 0;
    zx_gotoxy(1, MSG_ROW + 1);
    zx_setattr(INK_WHITE | PAPER_BLACK, 1);
    if (s) {
        while (s[n] && n < SCR_W) { zx_printchar(s[n]); n++; }
    }
    while (n < SCR_W) { zx_printchar(' '); n++; }
}

/* ---- full repaint ---- */
static void render(void)
{
    int i, r;

    cls_black();
    zx_border(INK_BLACK);

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
    for (;;) {
        k = in_inkey();
        if (k != 0) break;
    }
    wait_release();
    switch (k) {
    case 'q': case 'Q': return KEY_UP;
    case 'a': case 'A': return KEY_DOWN;
    case 'o': case 'O': return KEY_LEFT;
    case 'p': case 'P': return KEY_RIGHT;
    case ' ':           return KEY_SPACE;
    case 'n': case 'N': return KEY_NEW;
    case 'z': case 'Z': return KEY_UNDO;
    case 1:             return KEY_QUIT;   /* CAPS SHIFT + 1 (Esc) */
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

/* ---- one-line prompt helper ---- */
static int prompt_yes_replay_no(void)
{
    for (;;) {
        int k;
        strcpy(msg, "New game? Y/R/N");
        render();
        for (;;) { k = in_inkey(); if (k) break; }
        wait_release();
        if (k == 'y' || k == 'Y') return 1;
        if (k == 'r' || k == 'R') return 2;
        if (k == 'n' || k == 'N') return 0;
        if (k == 1) return 0;
    }
}

/* ---- main loop ---- */
int main(void)
{
    int quit = 0;

    zx_border(INK_BLACK);
    screen_init();

    srand(1);
    prepare_game(1);

    sel_row = 0; sel_col = 0; sel_depth = -1;
    selected = -1; selected_index = -1;
    strcpy(msg, "QAOP move SPACE sel N new Z undo");

    render();

    while (!quit) {
        int key = read_key();

        if (key == KEY_QUIT) { quit = 1; break; }

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
            int answer = prompt_yes_replay_no();
            if (answer) {
                free_game();
                if (answer == 1) prepare_game(make_seed());
                else             replay_game();
                sel_row = 0; sel_col = 0; sel_depth = -1;
                selected = -1; selected_index = -1;
                strcpy(msg, answer == 1 ? "New game." : "Replay.");
                render();
            } else {
                strcpy(msg, "QAOP move SPACE sel N new Z undo");
                render();
            }
            continue;
        }

        if (key == KEY_SPACE) {
            int slot = (sel_row == 0) ? sel_col : (SLOT_COL1 + sel_col);

            if (selected < 0) {
                if (slot == SLOT_STOCK) {
                    undo_push(sel_row, sel_col, sel_depth,
                              selected, selected_index);
                    strcpy(msg, try_move(SLOT_STOCK, SLOT_STOCK, -1));
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
                /* failed move: the undo snapshot is wasted, but v1
                   doesn't bother popping it */
            }
            if (game_won())
                strcpy(msg, "YOU WIN! N=new  Esc=quit");
            render();
            continue;
        }
    }

    cls_black();
    flush();
    return 0;
}