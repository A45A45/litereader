/*
 * Keys:
 * n / l / Space / → / ↓      next page
 * p / h / Backspace / ← / ↑  previous page
 * g                           first page
 * G                           last page
 * :<number> Enter             jump to page
 * q / Q                       quit  (progress auto-saved)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>

#define SIDE_MARGIN   3
#define LINE_BUF_CAP  65536

static struct termios g_orig_term;
static int g_raw_mode = 0;

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_term);
    write(STDOUT_FILENO, "\033[?25h", 6);
}

static void term_raw(void)
{
    if (g_raw_mode) return;
    tcgetattr(STDIN_FILENO, &g_orig_term);
    atexit(term_restore);
    struct termios raw = g_orig_term;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_raw_mode = 1;
}

static void term_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

typedef struct { char **v; int n, cap; } StrVec;

static void sv_init(StrVec *sv)
{
    sv->cap = 1024;
    sv->v   = malloc((size_t)sv->cap * sizeof(char *));
    sv->n   = 0;
}

static void sv_push(StrVec *sv, const char *s)
{
    if (sv->n >= sv->cap) {
        sv->cap *= 2;
        sv->v    = realloc(sv->v, (size_t)sv->cap * sizeof(char *));
    }
    sv->v[sv->n++] = strdup(s);
}

static void sv_free(StrVec *sv)
{
    for (int i = 0; i < sv->n; i++) free(sv->v[i]);
    free(sv->v);
}

static void wrap_line(StrVec *sv, const char *line, int width)
{
    int len = (int)strlen(line);
    if (len == 0) { sv_push(sv, ""); return; }
    if (width <= 0) width = 1;

    int start = 0;
    while (start < len) {
        int end = start + width;
        if (end >= len) { sv_push(sv, line + start); break; }

        int brk = end;
        while (brk > start && line[brk] != ' ') brk--;
        if (brk == start) brk = end;

        int chunk = brk - start;
        char *buf = malloc((size_t)chunk + 1);
        memcpy(buf, line + start, (size_t)chunk);
        buf[chunk] = '\0';
        sv_push(sv, buf);
        free(buf);

        start = brk;
        if (start < len && line[start] == ' ') start++;
    }
}

static int file_is_pdf(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char magic[4] = {0};
    int got = (int)fread(magic, 1, 4, f);
    fclose(f);
    return got == 4 &&
           magic[0] == '%' && magic[1] == 'P' &&
           magic[2] == 'D' && magic[3] == 'F';
}

static char *shell_quote(const char *s)
{
    size_t len = strlen(s);
    char  *out = malloc(len * 4 + 3);
    char  *p   = out;
    *p++ = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\'';
        } else {
            *p++ = s[i];
        }
    }
    *p++ = '\'';
    *p   = '\0';
    return out;
}

static int cmd_exists(const char *name)
{
    char buf[256];
    snprintf(buf, sizeof buf, "command -v %s >/dev/null 2>&1", name);
    return system(buf) == 0;
}

static FILE *open_pdf_stream(const char *path)
{
    char *qpath = shell_quote(path);
    char  cmd[4096];
    FILE *f = NULL;

    if (cmd_exists("pdftotext")) {
        snprintf(cmd, sizeof cmd, "pdftotext -layout %s - 2>/dev/null", qpath);
        f = popen(cmd, "r");
    } else if (cmd_exists("mutool")) {
        snprintf(cmd, sizeof cmd, "mutool draw -F text %s 2>/dev/null", qpath);
        f = popen(cmd, "r");
    } else if (cmd_exists("ps2ascii")) {
        snprintf(cmd, sizeof cmd, "ps2ascii %s 2>/dev/null", qpath);
        f = popen(cmd, "r");
    }

    free(qpath);
    return f;
}

static StrVec load_stream(FILE *f, int is_popen, int width)
{
    StrVec sv;
    sv_init(&sv);

    char *line = malloc(LINE_BUF_CAP);
    while (fgets(line, LINE_BUF_CAP, f)) {
        int n = (int)strlen(line);
        if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
        if (n > 0 && line[n - 1] == '\r') line[--n] = '\0';

        if (n > 0 && line[0] == '\f') {
            memmove(line, line + 1, (size_t)n--);
        }
        wrap_line(&sv, line, width);
    }
    free(line);

    if (is_popen) pclose(f);
    else          fclose(f);
    return sv;
}

static StrVec load_text(const char *path, int width)
{
    FILE *f = fopen(path, "r");
    if (!f) { term_restore(); perror(path); exit(1); }
    return load_stream(f, 0, width);
}

/* ── Save config and progress setup ── */
static void progress_path(const char *book, char *out, int outsz)
{
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) home = "."; 

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.litereader", home);
    mkdir(dir, 0755); 

    const char *base = strrchr(book, '/');
    base = base ? base + 1 : book;

    snprintf(out, (size_t)outsz, "%s/%s.progress", dir, base);
}

static int load_progress(const char *book)
{
    char p[4096];
    progress_path(book, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int page = 0;
    fscanf(f, "%d", &page);
    fclose(f);
    return page;
}

static void save_progress(const char *book, int page)
{
    char p[4096];
    progress_path(book, p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d\n", page);
    fclose(f);
}

#define ESC_CLEAR   "\033[2J\033[H"
#define ESC_INVERT  "\033[7m"
#define ESC_RESET   "\033[0m"

static void render(const StrVec *sv, int page,
                   int rows, int cols,
                   const char *title,
                   const char *cmd_buf)
{
    int content_rows = rows - 2;
    if (content_rows < 1) content_rows = 1;
    int total = (sv->n + content_rows - 1) / content_rows;
    if (total < 1) total = 1;

    write(STDOUT_FILENO, ESC_CLEAR, strlen(ESC_CLEAR));

    printf(ESC_INVERT " %-*.*s " ESC_RESET "\n", cols - 2, cols - 2, title);

    char pad[64];
    int margin = SIDE_MARGIN < (int)sizeof(pad) - 1
                 ? SIDE_MARGIN : (int)sizeof(pad) - 1;
    memset(pad, ' ', (size_t)margin);
    pad[margin] = '\0';

    int first = page * content_rows;
    int last  = first + content_rows;
    if (last > sv->n) last = sv->n;

    for (int i = first; i < last; i++)
        printf("%s%s\n", pad, sv->v[i]);
    for (int i = last - first; i < content_rows; i++)
        printf("\n");

    int pct = total > 1 ? (page * 100) / (total - 1) : 100;
    printf(ESC_INVERT);
    if (cmd_buf) {
        printf(" :%s%*s" ESC_RESET,
               cmd_buf, cols - (int)strlen(cmd_buf) - 3, "");
    } else {
        char left[256], right[256];
        snprintf(left,  sizeof left,
                 "  [n/p] page  [g/G] ends  [:<n>] jump  [q] quit  ");
        snprintf(right, sizeof right,
                 " %d/%d (%d%%) ", page + 1, total, pct);
        int gap = cols - (int)strlen(left) - (int)strlen(right);
        if (gap < 0) gap = 0;
        printf("%s%*s%s" ESC_RESET, left, gap, "", right);
    }
    fflush(stdout);
}

#define KEY_RIGHT 256
#define KEY_LEFT  257
#define KEY_UP    258
#define KEY_DOWN  259

static int read_key(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c != '\033') return c;

    unsigned char seq[2] = {0};
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
    if (seq[0] != '[')                        return '\033';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';

    switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
    }
    return '\033';
}

/* ── TUI File Picker Implementation ── */
static int cmp_file(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    const char *base_a = strrchr(sa, '/'); base_a = base_a ? base_a + 1 : sa;
    const char *base_b = strrchr(sb, '/'); base_b = base_b ? base_b + 1 : sb;
    return strcasecmp(base_a, base_b);
}

static void scan_downloads(StrVec *sv) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return;

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/Downloads", home);

    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue; // Skip hidden
        
        const char *ext = strrchr(dir->d_name, '.');
        if (ext && (strcasecmp(ext, ".pdf") == 0 || strcasecmp(ext, ".txt") == 0)) {
            char full_path[2048];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dir->d_name);
            sv_push(sv, full_path);
        }
    }
    closedir(d);

    if (sv->n > 0) {
        qsort(sv->v, sv->n, sizeof(char *), cmp_file);
    }
}

static char *file_picker(void) {
    StrVec files;
    sv_init(&files);
    scan_downloads(&files);

    if (files.n == 0) {
        sv_free(&files);
        return NULL;
    }

    int sel = 0, offset = 0;
    int rows, cols;
    char *chosen = NULL;

    while (1) {
        term_size(&rows, &cols);
        write(STDOUT_FILENO, ESC_CLEAR, strlen(ESC_CLEAR));

        printf(ESC_INVERT " %-*.*s " ESC_RESET "\n",
               cols - 2, cols - 2, "Select a file from ~/Downloads (q to quit, j/k to navigate)");

        int list_rows = rows - 2;
        if (list_rows < 1) list_rows = 1;

        if (sel < offset) offset = sel;
        if (sel >= offset + list_rows) offset = sel - list_rows + 1;

        for (int i = 0; i < list_rows; i++) {
            int idx = i + offset;
            if (idx >= files.n) break;

            const char *base = strrchr(files.v[idx], '/');
            base = base ? base + 1 : files.v[idx];

            if (idx == sel) {
                printf(ESC_INVERT " > %s " ESC_RESET "\n", base);
            } else {
                printf("   %s\n", base);
            }
        }

        int key = read_key();
        if (key == 'q' || key == 'Q' || key == '\033') {
            break;
        } else if (key == KEY_UP || key == 'k') {
            if (sel > 0) sel--;
        } else if (key == KEY_DOWN || key == 'j') {
            if (sel < files.n - 1) sel++;
        } else if (key == '\r' || key == '\n') {
            chosen = strdup(files.v[sel]);
            break;
        }
    }

    sv_free(&files);
    return chosen;
}

int main(int argc, char *argv[])
{
    char *picked_file = NULL;
    const char *path = NULL;

    term_raw();
    write(STDOUT_FILENO, "\033[?25l", 6);

    if (argc >= 2) {
        path = argv[1];
    } else {
        picked_file = file_picker();
        if (!picked_file) {
            write(STDOUT_FILENO, ESC_CLEAR, strlen(ESC_CLEAR));
            printf("No file selected or ~/Downloads is empty.\n");
            return 0;
        }
        path = picked_file;
    }

    int rows, cols;
    term_size(&rows, &cols);

    int wrap_width = cols - SIDE_MARGIN * 2;
    if (wrap_width < 20) wrap_width = 20;

    /* ── detect format and load ── */
    StrVec sv;

    if (file_is_pdf(path)) {
        FILE *pf = open_pdf_stream(path);
        if (!pf) {
            term_restore();
            fprintf(stderr,
                "Error: no PDF-to-text converter found on PATH.\n"
                "Install one of:\n"
                "  pdftotext  ->  sudo apt install poppler-utils  | brew install poppler\n"
                "  mutool     ->  sudo apt install mupdf-tools    | brew install mupdf-tools\n"
                "  ps2ascii   ->  sudo apt install ghostscript    | brew install ghostscript\n");
            if (picked_file) free(picked_file);
            return 1;
        }
        sv = load_stream(pf, 1 /* pclose */, wrap_width);
    } else {
        sv = load_text(path, wrap_width);
    }

    if (sv.n == 0) {
        term_restore();
        fprintf(stderr, "File is empty or could not be read.\n");
        if (picked_file) free(picked_file);
        return 1;
    }

    int content_rows = rows - 2;
    if (content_rows < 1) content_rows = 1;
    int total = (sv.n + content_rows - 1) / content_rows;

    int page = load_progress(path);
    if (page < 0)      page = 0;
    if (page >= total) page = total - 1;

    const char *title = strrchr(path, '/');
    title = title ? title + 1 : path;

    render(&sv, page, rows, cols, title, NULL);

    char cmd[32];
    int  cmd_len = 0, cmd_mode = 0;

    int key;
    while ((key = read_key()) != -1) {

        if (cmd_mode) {
            if (key >= '0' && key <= '9' && cmd_len < (int)sizeof(cmd) - 1) {
                cmd[cmd_len++] = (char)key;
                cmd[cmd_len]   = '\0';
            } else if (key == '\r' || key == '\n') {
                if (cmd_len > 0) {
                    int t = atoi(cmd) - 1;
                    if (t < 0)      t = 0;
                    if (t >= total) t = total - 1;
                    page = t;
                    save_progress(path, page);
                }
                cmd_mode = 0; cmd_len = 0; cmd[0] = '\0';
            } else if (key == '\033' || key == 'q') {
                cmd_mode = 0; cmd_len = 0; cmd[0] = '\0';
            }
            term_size(&rows, &cols);
            render(&sv, page, rows, cols, title, cmd_mode ? cmd : NULL);
            continue;
        }

        int old = page;
        switch (key) {
            case 'q': case 'Q': goto done;

            case 'n': case 'l': case ' ': case KEY_RIGHT: case KEY_DOWN:
                if (page < total - 1) page++;
                break;

            case 'p': case 'h': case '\b': case 127:
            case KEY_LEFT: case KEY_UP:
                if (page > 0) page--;
                break;

            case 'g': page = 0;         break;
            case 'G': page = total - 1; break;
            case ':': cmd_mode = 1; cmd_len = 0; cmd[0] = '\0'; break;
        }

        if (page != old) save_progress(path, page);

        term_size(&rows, &cols);
        content_rows = rows - 2;
        if (content_rows < 1) content_rows = 1;
        total = (sv.n + content_rows - 1) / content_rows;
        if (page >= total) page = total - 1;

        render(&sv, page, rows, cols, title, NULL);
    }

done:
    save_progress(path, page);
    sv_free(&sv);
    if (picked_file) free(picked_file);
    write(STDOUT_FILENO, ESC_CLEAR, strlen(ESC_CLEAR));
    printf("Progress saved at page %d. Goodbye!\n", page + 1);
    return 0;
}
