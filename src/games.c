#include "games.h"

#include "os_api.h"
#include "vga.h"

#define ATTR_BG 0x10
#define ATTR_TEXT 0x0F
#define ATTR_MUTED 0x07
#define ATTR_EDITOR_BG 0x07
#define ATTR_SNAKE 0x2A
#define ATTR_FOOD 0x4E
#define ATTR_PADDLE 0x1B
#define ATTR_BALL 0x4F
#define ATTR_TETRIS1 0x1E
#define ATTR_TETRIS2 0x2E
#define ATTR_TETRIS3 0x3E
#define ATTR_TETRIS4 0x4E

/* Support up to 160 cols × 60 rows — covers every standard VGA/SVGA text mode. */
#define GAME_SCREEN_MAX_COLS 160
#define GAME_SCREEN_MAX_ROWS  60
#define GAME_SCREEN_MAX_CELLS (GAME_SCREEN_MAX_COLS * GAME_SCREEN_MAX_ROWS)

enum game_id {
    GAME_NONE = 0,
    GAME_SNAKE,
    GAME_TETRIS,
    GAME_PINGPONG,
    GAME_EDITOR
};

struct snake_state {
    int length;
    int dir_x;
    int dir_y;
    int food_x;
    int food_y;
    int score;
    int body_x[128];
    int body_y[128];
};

struct pong_state {
    int left_y;
    int right_y;
    int ball_x;
    int ball_y;
    int vel_x;
    int vel_y;
    int player_score;
    int cpu_score;
};

struct tetris_piece {
    int type;
    int rotation;
    int x;
    int y;
};

struct tetris_state {
    int board[22][10];
    int board_height;
    int score;
    int game_over;
    struct tetris_piece piece;
};

struct editor_state {
    char path[INPUT_MAX + 1];
    char buffer[2048];
    uint32_t length;
    uint32_t cursor;
    char status[48];
};

static enum game_id active_game = GAME_NONE;
static uint32_t last_tick = 0;
static uint32_t rng_state = 0x12345678u;
static uint16_t saved_screen[GAME_SCREEN_MAX_CELLS];
static uint32_t saved_screen_rows = 0;
static uint32_t saved_screen_cols = 0;
static int saved_screen_valid = 0;
static struct snake_state snake;
static struct pong_state pong;
static struct tetris_state tetris;
static struct editor_state editor;
static int editor_frame_initialized = 0;
static uint32_t editor_frame_cols = 0;
static uint32_t editor_frame_rows = 0;

static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int str_eq(const char *a, const char *b)
{
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return 0;
        i++;
    }
    return a[i] == b[i];
}

static const char *path_basename(const char *path)
{
    const char *name = path;
    while (*path) {
        if (*path == '/')
            name = path + 1;
        path++;
    }
    return name;
}

static uint32_t next_rand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static void str_copy(char *dst, const char *src)
{
    while (*src) {
        *dst = *src;
        dst++;
        src++;
    }
    *dst = '\0';
}

static void u32_to_dec(uint32_t value, char *buffer)
{
    char temp[11];
    uint32_t count = 0;
    uint32_t i;

    if (value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    while (value > 0 && count < 10) {
        temp[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    for (i = 0; i < count; i++)
        buffer[i] = temp[count - i - 1];
    buffer[count] = '\0';
}

static void clear_screen(uint8_t attr)
{
    uint32_t row;
    for (row = 0; row < vga_rows(); row++)
        vga_clear_row(row, attr);
}

static void save_screen(void)
{
    uint32_t row;
    uint32_t col;
    uint32_t rows = vga_rows();
    uint32_t cols = vga_cols();

    if (rows > GAME_SCREEN_MAX_ROWS || cols > GAME_SCREEN_MAX_COLS) {
        saved_screen_valid = 0;
        return;
    }

    saved_screen_rows = rows;
    saved_screen_cols = cols;
    for (row = 0; row < rows; row++) {
        for (col = 0; col < cols; col++)
            saved_screen[row * cols + col] = vga_get_cell(row, col);
    }

    saved_screen_valid = 1;
}

static void restore_screen(void)
{
    uint32_t row;
    uint32_t col;

    if (!saved_screen_valid) {
        /* Can't restore — redraw the shell UI cleanly instead of leaving garbage. */
        os_term_clear();
        return;
    }

    for (row = 0; row < saved_screen_rows; row++) {
        for (col = 0; col < saved_screen_cols; col++)
            vga_set_cell(row, col, saved_screen[row * saved_screen_cols + col]);
    }
}

static void put_text(uint32_t row, uint32_t col, const char *text, uint8_t attr)
{
    vga_puts_at(text, attr, row, col);
}

static void center_text(uint32_t row, const char *text, uint8_t attr)
{
    uint32_t width = str_len(text);
    uint32_t cols = vga_cols();
    uint32_t col = 0;

    if (cols > width)
        col = (cols - width) / 2;

    put_text(row, col, text, attr);
}

static void game_draw_header(const char *title, uint32_t score)
{
    char score_buf[12];

    clear_screen(ATTR_BG);
    center_text(0, title, ATTR_TEXT);
    center_text(1, "Ctrl+C to exit", ATTR_MUTED);
    u32_to_dec(score, score_buf);
    put_text(0, 2, "Score: ", ATTR_TEXT);
    put_text(0, 9, score_buf, ATTR_TEXT);
}

static void editor_render(void)
{
    uint32_t cols = vga_cols();
    uint32_t rows = vga_rows();
    uint32_t usable_bottom = rows > 2 ? rows - 3 : rows;
    uint32_t r;
    uint32_t c = 0;
    uint32_t i = 0;
    uint32_t cursor_row = 4;
    uint32_t cursor_col = 0;
    int cursor_found = 0;

    if (!editor_frame_initialized || editor_frame_cols != cols || editor_frame_rows != rows) {
        clear_screen(ATTR_EDITOR_BG);
        put_text(0, 0, "Edit: ", ATTR_TEXT);
        put_text(0, 6, editor.path, ATTR_TEXT);
        center_text(1, "Text Editor", ATTR_MUTED);
        put_text(3, 0, "Ctrl+S Save  |  Ctrl+E Exit  |  Arrows Move  |  Enter New line", ATTR_TEXT);

        if (rows >= 2) {
            uint32_t help_col;
            for (help_col = 0; help_col < cols; help_col++)
                vga_put_at('-', ATTR_MUTED, rows - 2, help_col);
            for (help_col = 0; help_col < cols; help_col++)
                vga_put_at(' ', ATTR_TEXT, rows - 1, help_col);
            put_text(rows - 1, 0, " Ctrl+S: Save  |  Ctrl+E: Exit  |  Arrows: Move  |  Enter: New line ", ATTR_TEXT);
        }

        editor_frame_initialized = 1;
        editor_frame_cols = cols;
        editor_frame_rows = rows;
    }

    vga_clear_row(2, ATTR_EDITOR_BG);
    put_text(2, 0, editor.status, ATTR_MUTED);

    for (r = 4; r <= usable_bottom; r++)
        vga_clear_row(r, ATTR_EDITOR_BG);

    r = 4;

    while (r <= usable_bottom && i <= editor.length) {
        if (i == editor.cursor && !cursor_found) {
            cursor_row = r;
            cursor_col = c;
            cursor_found = 1;
        }

        if (i == editor.length)
            break;

        if (editor.buffer[i] == '\n') {
            r++;
            c = 0;
            i++;
            continue;
        }

        vga_put_at(editor.buffer[i], ATTR_TEXT, r, c);
        c++;
        if (c >= cols) {
            c = 0;
            r++;
        }
        i++;
    }

    if (!cursor_found) {
        cursor_row = r;
        cursor_col = c;
    }

    if (cursor_row <= usable_bottom) {
        uint16_t cell = vga_get_cell(cursor_row, cursor_col);
        uint8_t ch = (uint8_t)(cell & 0xFF);
        uint8_t attr = (uint8_t)((cell >> 8) & 0xFF);

        if (ch == ' ')
            vga_put_at('_', ATTR_TEXT, cursor_row, cursor_col);
        else
            vga_set_cell(cursor_row, cursor_col, vga_entry((char)ch, (uint8_t)((attr << 4) | (attr >> 4))));
    }
}

static void editor_move_vertical(int direction)
{
    uint32_t i;
    uint32_t line_start = 0;
    uint32_t col = 0;
    uint32_t target_start;
    uint32_t target_len = 0;

    for (i = 0; i < editor.cursor; i++) {
        if (editor.buffer[i] == '\n')
            line_start = i + 1;
    }

    col = editor.cursor - line_start;

    if (direction < 0) {
        uint32_t prev_end;
        if (line_start == 0)
            return;
        prev_end = line_start - 1;
        target_start = 0;
        for (i = 0; i < prev_end; i++) {
            if (editor.buffer[i] == '\n')
                target_start = i + 1;
        }
    } else {
        i = line_start;
        while (i < editor.length && editor.buffer[i] != '\n')
            i++;
        if (i >= editor.length)
            return;
        target_start = i + 1;
    }

    i = target_start;
    while (i < editor.length && editor.buffer[i] != '\n') {
        target_len++;
        i++;
    }

    editor.cursor = target_start + (col < target_len ? col : target_len);
}

static void editor_insert_char(char ch)
{
    uint32_t i;

    if (editor.length >= sizeof(editor.buffer) - 1)
        return;

    for (i = editor.length; i > editor.cursor; i--)
        editor.buffer[i] = editor.buffer[i - 1];

    editor.buffer[editor.cursor] = ch;
    editor.cursor++;
    editor.length++;
    editor.buffer[editor.length] = '\0';
}

static void editor_backspace(void)
{
    uint32_t i;

    if (editor.cursor == 0 || editor.length == 0)
        return;

    for (i = editor.cursor - 1; i < editor.length - 1; i++)
        editor.buffer[i] = editor.buffer[i + 1];

    editor.cursor--;
    editor.length--;
    editor.buffer[editor.length] = '\0';
}

static void snake_spawn_food(void)
{
    int cols = (int)vga_cols() - 2;
    int rows = (int)vga_rows() - 4;
    int ok = 0;

    while (!ok) {
        int i;
        snake.food_x = 1 + (int)(next_rand() % (uint32_t)(cols > 2 ? cols : 2));
        snake.food_y = 3 + (int)(next_rand() % (uint32_t)(rows > 2 ? rows : 2));
        ok = 1;
        for (i = 0; i < snake.length; i++) {
            if (snake.body_x[i] == snake.food_x && snake.body_y[i] == snake.food_y) {
                ok = 0;
                break;
            }
        }
    }
}

static void snake_render(void)
{
    uint32_t row;
    uint32_t col;
    uint32_t cols = vga_cols();
    uint32_t rows = vga_rows();
    int i;

    game_draw_header("Snake", (uint32_t)snake.score);

    for (col = 0; col < cols; col++) {
        vga_put_at('#', ATTR_TEXT, 2, col);
        vga_put_at('#', ATTR_TEXT, rows - 1, col);
    }
    for (row = 2; row < rows; row++) {
        vga_put_at('#', ATTR_TEXT, row, 0);
        vga_put_at('#', ATTR_TEXT, row, cols - 1);
    }

    vga_put_at('*', ATTR_FOOD, (uint32_t)snake.food_y, (uint32_t)snake.food_x);
    for (i = snake.length - 1; i >= 0; i--) {
        char ch = i == 0 ? '@' : 'o';
        vga_put_at(ch, ATTR_SNAKE, (uint32_t)snake.body_y[i], (uint32_t)snake.body_x[i]);
    }
}

static void snake_reset(void)
{
    uint32_t cols = vga_cols();
    uint32_t rows = vga_rows();

    snake.length = 4;
    snake.dir_x = 1;
    snake.dir_y = 0;
    snake.score = 0;
    snake.body_x[0] = (int)(cols / 2);
    snake.body_y[0] = (int)(rows / 2);
    snake.body_x[1] = snake.body_x[0] - 1;
    snake.body_y[1] = snake.body_y[0];
    snake.body_x[2] = snake.body_x[1] - 1;
    snake.body_y[2] = snake.body_y[0];
    snake.body_x[3] = snake.body_x[2] - 1;
    snake.body_y[3] = snake.body_y[0];
    snake_spawn_food();
    snake_render();
}

static void snake_step(void)
{
    int next_x = snake.body_x[0] + snake.dir_x;
    int next_y = snake.body_y[0] + snake.dir_y;
    int grow = 0;
    int i;

    if (next_x <= 0 || next_x >= (int)vga_cols() - 1 || next_y <= 2 || next_y >= (int)vga_rows() - 1) {
        snake_reset();
        return;
    }

    for (i = 0; i < snake.length; i++) {
        if (snake.body_x[i] == next_x && snake.body_y[i] == next_y) {
            snake_reset();
            return;
        }
    }

    if (next_x == snake.food_x && next_y == snake.food_y) {
        grow = 1;
        if (snake.length < 127)
            snake.length++;
        snake.score += 10;
        snake_spawn_food();
    }

    for (i = snake.length - 1; i > 0; i--) {
        snake.body_x[i] = snake.body_x[i - 1];
        snake.body_y[i] = snake.body_y[i - 1];
    }
    snake.body_x[0] = next_x;
    snake.body_y[0] = next_y;

    if (!grow && snake.length > 4 && snake.length > 127)
        snake.length = 127;

    snake_render();
}

static void pong_render(void)
{
    uint32_t row;
    uint32_t cols = vga_cols();
    uint32_t rows = vga_rows();
    char score_text[16];
    char left[12];
    char right[12];
    uint32_t i = 0;

    clear_screen(ATTR_BG);
    center_text(0, "Ping Pong", ATTR_TEXT);
    center_text(1, "Ctrl+C to exit | Up/Down move paddle", ATTR_MUTED);

    u32_to_dec((uint32_t)pong.player_score, left);
    u32_to_dec((uint32_t)pong.cpu_score, right);
    while (left[i]) {
        score_text[i] = left[i];
        i++;
    }
    score_text[i++] = ' ';
    score_text[i++] = ':';
    score_text[i++] = ' ';
    {
        uint32_t j = 0;
        while (right[j]) {
            score_text[i++] = right[j++];
        }
    }
    score_text[i] = '\0';
    center_text(2, score_text, ATTR_TEXT);

    for (row = 3; row < rows; row++)
        vga_put_at('|', ATTR_MUTED, row, cols / 2);

    for (row = 0; row < 4; row++) {
        int ly = pong.left_y + (int)row;
        int ry = pong.right_y + (int)row;
        if (ly >= 3 && ly < (int)rows)
            vga_put_at('#', ATTR_PADDLE, (uint32_t)ly, 2);
        if (ry >= 3 && ry < (int)rows)
            vga_put_at('#', ATTR_PADDLE, (uint32_t)ry, cols - 3);
    }

    vga_put_at('O', ATTR_BALL, (uint32_t)pong.ball_y, (uint32_t)pong.ball_x);
}

static void pong_reset_positions(int ball_dir)
{
    pong.left_y = (int)(vga_rows() / 2) - 2;
    pong.right_y = pong.left_y;
    pong.ball_x = (int)(vga_cols() / 2);
    pong.ball_y = (int)(vga_rows() / 2);
    pong.vel_x = ball_dir;
    pong.vel_y = ((int)(next_rand() % 3u)) - 1;
}

static void pong_reset(void)
{
    pong.player_score = 0;
    pong.cpu_score = 0;
    pong_reset_positions(1);
    pong_render();
}

static void pong_step(void)
{
    int next_x = pong.ball_x + pong.vel_x;
    int next_y = pong.ball_y + pong.vel_y;
    int paddle_center = pong.right_y + 1;

    if (pong.ball_y < paddle_center)
        pong.right_y--;
    else if (pong.ball_y > paddle_center)
        pong.right_y++;

    if (pong.right_y < 3)
        pong.right_y = 3;
    if (pong.right_y > (int)vga_rows() - 4)
        pong.right_y = (int)vga_rows() - 4;

    if (next_y <= 3 || next_y >= (int)vga_rows() - 1) {
        pong.vel_y = -pong.vel_y;
        next_y = pong.ball_y + pong.vel_y;
    }

    if (next_x == 3 && next_y >= pong.left_y && next_y < pong.left_y + 4) {
        pong.vel_x = 1;
        pong.vel_y = next_y - (pong.left_y + 1);
        next_x = pong.ball_x + pong.vel_x;
    }
    if (next_x == (int)vga_cols() - 4 && next_y >= pong.right_y && next_y < pong.right_y + 4) {
        pong.vel_x = -1;
        pong.vel_y = next_y - (pong.right_y + 1);
        next_x = pong.ball_x + pong.vel_x;
    }

    if (next_x <= 0) {
        pong.cpu_score++;
        pong_reset_positions(1);
        pong_render();
        return;
    }
    if (next_x >= (int)vga_cols() - 1) {
        pong.player_score++;
        pong_reset_positions(-1);
        pong_render();
        return;
    }

    pong.ball_x = next_x;
    pong.ball_y = next_y;
    pong_render();
}

static const int tetrominoes[4][4][4][2] = {
    {
        {{0,0},{1,0},{0,1},{1,1}},
        {{0,0},{1,0},{0,1},{1,1}},
        {{0,0},{1,0},{0,1},{1,1}},
        {{0,0},{1,0},{0,1},{1,1}}
    },
    {
        {{0,0},{1,0},{2,0},{3,0}},
        {{1,-1},{1,0},{1,1},{1,2}},
        {{0,1},{1,1},{2,1},{3,1}},
        {{2,-1},{2,0},{2,1},{2,2}}
    },
    {
        {{1,0},{0,1},{1,1},{2,1}},
        {{1,0},{1,1},{2,1},{1,2}},
        {{0,1},{1,1},{2,1},{1,2}},
        {{1,0},{0,1},{1,1},{1,2}}
    },
    {
        {{0,0},{0,1},{1,1},{2,1}},
        {{1,0},{2,0},{1,1},{1,2}},
        {{0,1},{1,1},{2,1},{2,2}},
        {{1,0},{1,1},{0,2},{1,2}}
    }
};

static int tetris_can_place(int type, int rotation, int x, int y)
{
    int i;
    for (i = 0; i < 4; i++) {
        int px = x + tetrominoes[type][rotation][i][0];
        int py = y + tetrominoes[type][rotation][i][1];
        if (px < 0 || px >= 10 || py >= tetris.board_height)
            return 0;
        if (py >= 0 && tetris.board[py][px])
            return 0;
    }
    return 1;
}

static void tetris_spawn_piece(void)
{
    tetris.piece.type = (int)(next_rand() % 4u);
    tetris.piece.rotation = 0;
    tetris.piece.x = 3;
    tetris.piece.y = 0;
    if (!tetris_can_place(tetris.piece.type, tetris.piece.rotation, tetris.piece.x, tetris.piece.y))
        tetris.game_over = 1;
}

static void tetris_clear_lines(void)
{
    int y;
    for (y = tetris.board_height - 1; y >= 0; y--) {
        int x;
        int full = 1;
        for (x = 0; x < 10; x++) {
            if (!tetris.board[y][x]) {
                full = 0;
                break;
            }
        }
        if (full) {
            int yy;
            for (yy = y; yy > 0; yy--) {
                for (x = 0; x < 10; x++)
                    tetris.board[yy][x] = tetris.board[yy - 1][x];
            }
            for (x = 0; x < 10; x++)
                tetris.board[0][x] = 0;
            tetris.score += 100;
            y++;
        }
    }
}

static void tetris_lock_piece(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        int px = tetris.piece.x + tetrominoes[tetris.piece.type][tetris.piece.rotation][i][0];
        int py = tetris.piece.y + tetrominoes[tetris.piece.type][tetris.piece.rotation][i][1];
        if (py >= 0 && py < tetris.board_height && px >= 0 && px < 10)
            tetris.board[py][px] = tetris.piece.type + 1;
    }
    tetris_clear_lines();
    tetris_spawn_piece();
}

static void tetris_render(void)
{
    int x;
    int y;
    uint32_t cols = vga_cols();
    uint32_t rows = vga_rows();
    int origin_x = (int)(cols / 2) - 6;
    int origin_y = 3;
    (void)rows;

    game_draw_header("Tetris", (uint32_t)tetris.score);

    for (y = 0; y <= tetris.board_height; y++) {
        vga_put_at('|', ATTR_TEXT, (uint32_t)(origin_y + y), (uint32_t)origin_x);
        vga_put_at('|', ATTR_TEXT, (uint32_t)(origin_y + y), (uint32_t)(origin_x + 11));
    }
    for (x = 0; x < 12; x++)
        vga_put_at('-', ATTR_TEXT, (uint32_t)(origin_y + tetris.board_height), (uint32_t)(origin_x + x));

    for (y = 0; y < tetris.board_height; y++) {
        for (x = 0; x < 10; x++) {
            int cell = tetris.board[y][x];
            if (cell) {
                uint8_t attr = (uint8_t)(ATTR_TETRIS1 + (cell - 1) * 0x10);
                vga_put_at('#', attr, (uint32_t)(origin_y + y), (uint32_t)(origin_x + 1 + x));
            } else {
                vga_put_at(' ', ATTR_BG, (uint32_t)(origin_y + y), (uint32_t)(origin_x + 1 + x));
            }
        }
    }

    if (!tetris.game_over) {
        for (x = 0; x < 4; x++) {
            int px = tetris.piece.x + tetrominoes[tetris.piece.type][tetris.piece.rotation][x][0];
            int py = tetris.piece.y + tetrominoes[tetris.piece.type][tetris.piece.rotation][x][1];
            if (py >= 0)
                vga_put_at('@', (uint8_t)(ATTR_TETRIS1 + tetris.piece.type * 0x10), (uint32_t)(origin_y + py), (uint32_t)(origin_x + 1 + px));
        }
    } else {
        center_text((uint32_t)(origin_y + 2), "GAME OVER", ATTR_TEXT);
        center_text((uint32_t)(origin_y + 3), "Ctrl+C to return", ATTR_MUTED);
    }
}

static void tetris_reset(void)
{
    int y;
    int x;
    tetris.board_height = (int)(vga_rows() > 8 ? vga_rows() - 5 : 10);
    if (tetris.board_height > 22)
        tetris.board_height = 22;
    if (tetris.board_height < 10)
        tetris.board_height = 10;
    tetris.score = 0;
    tetris.game_over = 0;
    for (y = 0; y < 22; y++) {
        for (x = 0; x < 10; x++)
            tetris.board[y][x] = 0;
    }
    tetris_spawn_piece();
    tetris_render();
}

static void tetris_step_down(void)
{
    if (tetris.game_over)
        return;
    if (tetris_can_place(tetris.piece.type, tetris.piece.rotation, tetris.piece.x, tetris.piece.y + 1))
        tetris.piece.y++;
    else
        tetris_lock_piece();
    tetris_render();
}

static void tetris_try_rotate(int direction)
{
    static const int kicks[] = { 0, -1, 1, -2, 2 };
    int next_rotation = (tetris.piece.rotation + direction) & 3;
    uint32_t index;

    for (index = 0; index < (uint32_t)(sizeof(kicks) / sizeof(kicks[0])); index++) {
        int next_x = tetris.piece.x + kicks[index];

        if (tetris_can_place(tetris.piece.type, next_rotation, next_x, tetris.piece.y)) {
            tetris.piece.rotation = next_rotation;
            tetris.piece.x = next_x;
            return;
        }
    }
}

int games_launch_snake(void)
{
    save_screen();
    rng_state ^= (uint32_t)vga_cols() << 16;
    rng_state ^= (uint32_t)vga_rows();
    active_game = GAME_SNAKE;
    last_tick = 0;
    snake_reset();
    return 1;
}

int games_launch_tetris(void)
{
    save_screen();
    rng_state ^= (uint32_t)vga_cols() << 16;
    rng_state ^= (uint32_t)vga_rows();
    active_game = GAME_TETRIS;
    last_tick = 0;
    tetris_reset();
    return 1;
}

int games_launch_pingpong(void)
{
    save_screen();
    rng_state ^= (uint32_t)vga_cols() << 16;
    rng_state ^= (uint32_t)vga_rows();
    active_game = GAME_PINGPONG;
    last_tick = 0;
    pong_reset();
    return 1;
}

int games_start_editor(const char *path)
{
    const char *content = os_fs_get_file_content(path);
    uint32_t i = 0;

    os_cursor_hide();
    save_screen();
    active_game = GAME_EDITOR;
    last_tick = 0;
    editor_frame_initialized = 0;

    str_copy(editor.path, path);
    editor.length = 0;
    editor.cursor = 0;
    str_copy(editor.status, "Ready");

    if (content) {
        while (content[i] && i < sizeof(editor.buffer) - 1) {
            editor.buffer[i] = content[i];
            i++;
        }
        editor.length = i;
    }
    editor.buffer[editor.length] = '\0';

    editor_render();
    return 1;
}

int games_start_path(const char *path)
{
    const char *name = path_basename(path);

    if (str_eq(name, "snake"))
        return games_launch_snake();
    if (str_eq(name, "tetris"))
        return games_launch_tetris();
    if (str_eq(name, "pingpong"))
        return games_launch_pingpong();

    return 0;
}

int games_is_active(void)
{
    return active_game != GAME_NONE && active_game != GAME_EDITOR;
}

int games_editor_is_active(void)
{
    return active_game == GAME_EDITOR;
}

void games_handle_event(const struct key_event *event)
{
    if (active_game == GAME_NONE)
        return;

    if (active_game == GAME_EDITOR) {
        if (event->type == KEY_CTRL_E) {
            restore_screen();
            active_game = GAME_NONE;
            os_redraw_input_line();
            os_cursor_reset_blink();
            return;
        }

        if (event->type == KEY_CTRL_S) {
            if (os_fs_write_file(editor.path, editor.buffer))
                str_copy(editor.status, "Saved");
            else
                str_copy(editor.status, "Save failed");
            editor_render();
            return;
        }

        if (event->type == KEY_CHAR) {
            editor_insert_char(event->ch);
            editor_render();
            return;
        }

        if (event->type == KEY_ENTER) {
            editor_insert_char('\n');
            editor_render();
            return;
        }

        if (event->type == KEY_TAB) {
            editor_insert_char(' ');
            editor_insert_char(' ');
            editor_insert_char(' ');
            editor_insert_char(' ');
            editor_render();
            return;
        }

        if (event->type == KEY_BACKSPACE) {
            editor_backspace();
            editor_render();
            return;
        }

        if (event->type == KEY_LEFT) {
            if (editor.cursor > 0)
                editor.cursor--;
            editor_render();
            return;
        }

        if (event->type == KEY_RIGHT) {
            if (editor.cursor < editor.length)
                editor.cursor++;
            editor_render();
            return;
        }

        if (event->type == KEY_UP) {
            editor_move_vertical(-1);
            editor_render();
            return;
        }

        if (event->type == KEY_DOWN) {
            editor_move_vertical(1);
            editor_render();
            return;
        }

        return;
    }

    if (event->type == KEY_CTRL_C) {
        restore_screen();
        active_game = GAME_NONE;
        return;
    }

    if (active_game == GAME_SNAKE) {
        if (event->type == KEY_LEFT && snake.dir_x != 1) {
            snake.dir_x = -1;
            snake.dir_y = 0;
        } else if (event->type == KEY_RIGHT && snake.dir_x != -1) {
            snake.dir_x = 1;
            snake.dir_y = 0;
        } else if (event->type == KEY_UP && snake.dir_y != 1) {
            snake.dir_x = 0;
            snake.dir_y = -1;
        } else if (event->type == KEY_DOWN && snake.dir_y != -1) {
            snake.dir_x = 0;
            snake.dir_y = 1;
        }
    } else if (active_game == GAME_PINGPONG) {
        if (event->type == KEY_UP)
            pong.left_y--;
        else if (event->type == KEY_DOWN)
            pong.left_y++;

        if (pong.left_y < 3)
            pong.left_y = 3;
        if (pong.left_y > (int)vga_rows() - 4)
            pong.left_y = (int)vga_rows() - 4;
        pong_render();
    } else if (active_game == GAME_TETRIS) {
        if (tetris.game_over)
            return;
        if (event->type == KEY_LEFT) {
            if (tetris_can_place(tetris.piece.type, tetris.piece.rotation, tetris.piece.x - 1, tetris.piece.y))
                tetris.piece.x--;
        } else if (event->type == KEY_RIGHT) {
            if (tetris_can_place(tetris.piece.type, tetris.piece.rotation, tetris.piece.x + 1, tetris.piece.y))
                tetris.piece.x++;
        } else if (event->type == KEY_DOWN) {
            tetris_step_down();
            return;
        } else if (event->type == KEY_UP) {
            tetris_try_rotate(1);
        } else if (event->type == KEY_CHAR && (event->ch == 'z' || event->ch == 'Z')) {
            tetris_try_rotate(-1);
        } else if (event->type == KEY_CHAR &&
                   (event->ch == 'x' || event->ch == 'X' || event->ch == 'w' || event->ch == 'W')) {
            tetris_try_rotate(1);
        }
        tetris_render();
    }
}

void games_tick(uint32_t ticks)
{
    uint32_t interval;

    if (active_game == GAME_NONE)
        return;

    if (active_game == GAME_EDITOR)
        return;

    interval = active_game == GAME_TETRIS ? 8u : 4u;
    if (ticks == last_tick || ticks - last_tick < interval)
        return;

    last_tick = ticks;

    if (active_game == GAME_SNAKE)
        snake_step();
    else if (active_game == GAME_PINGPONG)
        pong_step();
    else if (active_game == GAME_TETRIS)
        tetris_step_down();
}
