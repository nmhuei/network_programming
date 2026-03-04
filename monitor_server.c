/*
 * monitor_server.c — Remote System Monitor Server
 *
 * Dashboard real-time trên terminal, nhận metrics từ agents qua TCP,
 * hỗ trợ viewer commands (/view, /history, /log, /help).
 *
 * Build:  gcc -Wall -o monitor_server monitor_server.c -lm
 * Usage:  ./monitor_server [--port PORT] [--host HOST] [--config FILE]
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>

/* ─── Constants ───────────────────────────────────────────────────── */
#define MAX_AGENTS         6
#define OFFLINE_TIMEOUT    30
#define HISTORY_MAXLEN     10000
#define EVENT_LOG_MAXLEN   50
#define DASHBOARD_MIN_ROWS 12
#define DASHBOARD_MAX_ROWS 40
#define MIN_TERM_WIDTH     119
#define MIN_TERM_HEIGHT    12
#define MAX_CLIENTS        64
#define BUF_SIZE           4096
#define NAME_LEN           64
#define ALERT_LEN          256
#define EVENT_MSG_LEN      256
#define LINE_BUF_LEN       4096
#define DEFAULT_PORT       8784
#define REFRESH_INTERVAL   5

/* Column widths for dashboard table */
#define COL_HOST   20
#define COL_CPU    16
#define COL_RAM    16
#define COL_DISK   16
#define COL_STATUS 12
#define COL_ALERT  32

/* ─── ANSI Colors ─────────────────────────────────────────────────── */
#define RST       "\033[0m"
#define BOLD      "\033[1m"
#define DIM       "\033[2m"
#define RED       "\033[31m"
#define GREEN     "\033[32m"
#define YELLOW    "\033[33m"
#define CYAN      "\033[36m"
#define BRED      "\033[91m"
#define BGREEN    "\033[92m"
#define BYELLOW   "\033[93m"
#define BCYAN     "\033[96m"
#define BOLD_RED  "\033[1;91m"
#define BOLD_GRN  "\033[1;92m"
#define BOLD_YEL  "\033[1;93m"
#define CLR_SCR   "\033[2J\033[H"
#define HIDE_CUR  "\033[?25l"
#define SHOW_CUR  "\033[?25h"

/* ─── Data Structures ─────────────────────────────────────────────── */

typedef struct {
    double cpu, ram, disk;
    time_t timestamp;
} HistoryEntry;

typedef struct {
    char     name[NAME_LEN];
    double   cpu, ram, disk;
    time_t   last_seen;
    int      online;          /* 1 = connected */
    int      fd;              /* socket fd, -1 if disconnected */
    char     alert[ALERT_LEN];
    /* history circular buffer */
    HistoryEntry *history;
    int      hist_count;
    int      hist_head;       /* next write position */
} HostInfo;

typedef struct {
    time_t timestamp;
    char   msg[EVENT_MSG_LEN];
} EventEntry;

typedef struct {
    char   name[NAME_LEN];   /* "" = global */
    double cpu, ram, disk;
} ThresholdEntry;

typedef enum {
    CLIENT_UNKNOWN,
    CLIENT_AGENT,
    CLIENT_VIEWER
} ClientType;

typedef struct {
    int        fd;
    ClientType type;
    char       line_buf[LINE_BUF_LEN];
    int        line_len;
    char       addr[64];
    int        host_idx;      /* index into hosts[] if agent, -1 otherwise */
} ClientInfo;

/* ─── Global State ────────────────────────────────────────────────── */
static HostInfo      hosts[MAX_AGENTS];
static int           host_count = 0;          /* unique host names seen */

static EventEntry    event_log[EVENT_LOG_MAXLEN];
static int           event_count = 0;
static int           event_head  = 0;

static ThresholdEntry thresholds[MAX_AGENTS + 1]; /* [0] = global + per-host */
static int            threshold_count = 0;

static ClientInfo    clients[MAX_CLIENTS];
static int           client_count = 0;

static volatile sig_atomic_t g_need_redraw = 1;
static volatile sig_atomic_t g_running     = 1;
static int           term_width  = 80;
static int           term_height = 24;

static struct termios orig_termios;
static int            termios_saved = 0;

/* ─── Utility ─────────────────────────────────────────────────────── */

static void get_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_width  = ws.ws_col;
        term_height = ws.ws_row;
    }
}

static void sigwinch_handler(int sig) {
    (void)sig;
    g_need_redraw = 1;
}

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void restore_terminal(void) {
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
    printf(SHOW_CUR RST "\n");
    fflush(stdout);
}

static void setup_terminal(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
        termios_saved = 1;
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    printf(HIDE_CUR);
    fflush(stdout);
}

static void add_event(const char *fmt, ...) {
    EventEntry *e = &event_log[event_head];
    e->timestamp = time(NULL);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, EVENT_MSG_LEN, fmt, ap);
    va_end(ap);
    event_head = (event_head + 1) % EVENT_LOG_MAXLEN;
    if (event_count < EVENT_LOG_MAXLEN) event_count++;
}

/* Fixed add_event without nested include */
/* We'll use stdarg properly — see corrected version below */

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ─── Threshold Config Parser ─────────────────────────────────────── */

static void load_default_thresholds(void) {
    threshold_count = 1;
    memset(&thresholds[0], 0, sizeof(ThresholdEntry));
    thresholds[0].name[0] = '\0'; /* global */
    thresholds[0].cpu  = 80.0;
    thresholds[0].ram  = 90.0;
    thresholds[0].disk = 85.0;
}

static void parse_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        load_default_thresholds();
        return;
    }
    load_default_thresholds();

    char line[256];
    int  cur_idx = 0; /* index into thresholds, 0 = global */

    while (fgets(line, sizeof(line), f)) {
        /* trim */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        /* skip empty / comments */
        if (*p == '\0' || *p == '\n' || *p == '#' || *p == ';') continue;

        if (*p == '[') {
            /* section header */
            char *end = strchr(p, ']');
            if (!end) continue;
            *end = '\0';
            p++;
            if (strcasecmp(p, "global") == 0) {
                cur_idx = 0;
            } else if (strncmp(p, "host:", 5) == 0) {
                const char *hname = p + 5;
                /* find or create threshold entry */
                int found = -1;
                for (int i = 1; i < threshold_count; i++) {
                    if (strcmp(thresholds[i].name, hname) == 0) {
                        found = i; break;
                    }
                }
                if (found >= 0) {
                    cur_idx = found;
                } else if (threshold_count < MAX_AGENTS + 1) {
                    cur_idx = threshold_count++;
                    strncpy(thresholds[cur_idx].name, hname, NAME_LEN - 1);
                    /* inherit global defaults */
                    thresholds[cur_idx].cpu  = thresholds[0].cpu;
                    thresholds[cur_idx].ram  = thresholds[0].ram;
                    thresholds[cur_idx].disk = thresholds[0].disk;
                }
            }
        } else {
            /* key = value */
            char key[32], val_s[32];
            if (sscanf(p, "%31[^ =\t] = %31s", key, val_s) == 2) {
                double val = atof(val_s);
                if (strcasecmp(key, "cpu") == 0)       thresholds[cur_idx].cpu  = val;
                else if (strcasecmp(key, "ram") == 0)   thresholds[cur_idx].ram  = val;
                else if (strcasecmp(key, "disk") == 0)  thresholds[cur_idx].disk = val;
            }
        }
    }
    fclose(f);
}

static void get_thresholds(const char *hostname, double *cpu, double *ram, double *disk) {
    /* start with global */
    *cpu  = thresholds[0].cpu;
    *ram  = thresholds[0].ram;
    *disk = thresholds[0].disk;
    /* override with per-host if found */
    for (int i = 1; i < threshold_count; i++) {
        if (strcmp(thresholds[i].name, hostname) == 0) {
            *cpu  = thresholds[i].cpu;
            *ram  = thresholds[i].ram;
            *disk = thresholds[i].disk;
            return;
        }
    }
}

/* ─── Host Management ─────────────────────────────────────────────── */

static int find_host(const char *name) {
    for (int i = 0; i < host_count; i++) {
        if (strcmp(hosts[i].name, name) == 0) return i;
    }
    return -1;
}

static int add_host(const char *name) {
    if (host_count >= MAX_AGENTS) return -1;
    int idx = host_count++;
    memset(&hosts[idx], 0, sizeof(HostInfo));
    strncpy(hosts[idx].name, name, NAME_LEN - 1);
    hosts[idx].fd = -1;
    hosts[idx].history = calloc(HISTORY_MAXLEN, sizeof(HistoryEntry));
    hosts[idx].hist_count = 0;
    hosts[idx].hist_head  = 0;
    return idx;
}

static void add_history(int idx, double cpu, double ram, double disk, time_t ts) {
    HostInfo *h = &hosts[idx];
    HistoryEntry *e = &h->history[h->hist_head];
    e->cpu  = cpu;
    e->ram  = ram;
    e->disk = disk;
    e->timestamp = ts;
    h->hist_head = (h->hist_head + 1) % HISTORY_MAXLEN;
    if (h->hist_count < HISTORY_MAXLEN) h->hist_count++;
}

/* ─── Client Management ──────────────────────────────────────────── */

static ClientInfo* add_client(int fd, const char *addr) {
    if (client_count >= MAX_CLIENTS) return NULL;
    ClientInfo *c = &clients[client_count++];
    memset(c, 0, sizeof(ClientInfo));
    c->fd = fd;
    c->type = CLIENT_UNKNOWN;
    c->host_idx = -1;
    strncpy(c->addr, addr, sizeof(c->addr) - 1);
    return c;
}

static void remove_client(int idx) {
    if (idx < 0 || idx >= client_count) return;
    ClientInfo *c = &clients[idx];

    if (c->type == CLIENT_AGENT && c->host_idx >= 0 && c->host_idx < host_count) {
        hosts[c->host_idx].online = 0;
        hosts[c->host_idx].fd = -1;
        add_event("Agent '%s' disconnected", hosts[c->host_idx].name);
        g_need_redraw = 1;
    } else if (c->type == CLIENT_VIEWER) {
        add_event("Viewer %s disconnected", c->addr);
    }

    close(c->fd);
    /* shift array */
    for (int i = idx; i < client_count - 1; i++) {
        clients[i] = clients[i + 1];
    }
    client_count--;
}



/* ─── Send to client ──────────────────────────────────────────────── */

static void send_to_fd(int fd, const char *data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += n;
    }
}

static void send_str(int fd, const char *str) {
    send_to_fd(fd, str, strlen(str));
}

static void broadcast_viewers(const char *msg) {
    for (int i = 0; i < client_count; i++) {
        if (clients[i].type == CLIENT_VIEWER) {
            send_str(clients[i].fd, msg);
        }
    }
}

/* ─── Alert Checking ──────────────────────────────────────────────── */

static void check_alerts(int host_idx) {
    HostInfo *h = &hosts[host_idx];
    double t_cpu, t_ram, t_disk;
    get_thresholds(h->name, &t_cpu, &t_ram, &t_disk);

    char parts[3][128];
    int  nparts = 0;

    if (h->cpu >= t_cpu) {
        snprintf(parts[nparts++], 128, "CPU=%.1f%% (threshold=%.0f%%)", h->cpu, t_cpu);
    }
    if (h->ram >= t_ram) {
        snprintf(parts[nparts++], 128, "RAM=%.1f%% (threshold=%.0f%%)", h->ram, t_ram);
    }
    if (h->disk >= t_disk) {
        snprintf(parts[nparts++], 128, "DISK=%.1f%% (threshold=%.0f%%)", h->disk, t_disk);
    }

    if (nparts > 0) {
        /* build alert string */
        char detail[ALERT_LEN] = "";
        for (int i = 0; i < nparts; i++) {
            if (i > 0) strcat(detail, ", ");
            strcat(detail, parts[i]);
        }

        /* short version for dashboard */
        strncpy(h->alert, parts[0], ALERT_LEN - 1);

        /* broadcast */
        struct tm tm_now;
        time_t now = time(NULL);
        localtime_r(&now, &tm_now);
        char broadcast[512];
        snprintf(broadcast, sizeof(broadcast),
                 "*** [%02d:%02d:%02d] ALERT host=%s: %s ***\r\n",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 h->name, detail);
        broadcast_viewers(broadcast);
        add_event("ALERT host=%s: %s", h->name, detail);
    } else {
        h->alert[0] = '\0';
    }
}

/* ─── JSON Parser (minimal) ──────────────────────────────────────── */

static int json_get_string(const char *json, const char *key, char *out, int maxlen) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_get_double(const char *json, const char *key, double *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    *out = strtod(p, NULL);
    return 0;
}

static int json_get_long(const char *json, const char *key, long *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    *out = strtol(p, NULL, 10);
    return 0;
}

/* ─── Dashboard Rendering ─────────────────────────────────────────── */

static const char* color_for_val(double val) {
    if (val >= 85.0) return BRED;
    if (val >= 70.0) return BYELLOW;
    return BGREEN;
}

static void render_progress_bar(char *buf, int buflen, double val, int bar_width) {
    int filled = (int)round(val / 100.0 * bar_width);
    if (filled < 0) filled = 0;
    if (filled > bar_width) filled = bar_width;
    const char *color = color_for_val(val);

    int pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "%s%5.1f ", color, val);
    for (int i = 0; i < filled && pos < buflen - 10; i++)
        pos += snprintf(buf + pos, buflen - pos, "█");
    pos += snprintf(buf + pos, buflen - pos, DIM);
    for (int i = filled; i < bar_width && pos < buflen - 10; i++)
        pos += snprintf(buf + pos, buflen - pos, "░");
    pos += snprintf(buf + pos, buflen - pos, RST);
}

/* Compute visible length of string (without ANSI escapes) */
static int visible_len(const char *s) {
    int len = 0;
    int in_esc = 0;
    while (*s) {
        if (*s == '\033') { in_esc = 1; }
        else if (in_esc && ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) { in_esc = 0; }
        else if (!in_esc) {
            /* count UTF-8 characters; box-drawing chars are 3 bytes */
            if ((*s & 0xC0) != 0x80) len++;
        }
        s++;
    }
    return len;
}

/* Pad string with spaces to reach target visible width */
static void pad_to(char *buf, int buflen, int target_vis_width) {
    int vl = visible_len(buf);
    int pos = strlen(buf);
    while (vl < target_vis_width && pos < buflen - 1) {
        buf[pos++] = ' ';
        vl++;
    }
    buf[pos] = '\0';
}

static void draw_dashboard(void) {
    get_term_size();

    printf(CLR_SCR);

    if (term_width < MIN_TERM_WIDTH || term_height < MIN_TERM_HEIGHT) {
        /* Terminal too small warning */
        int y = term_height / 2 - 1;
        for (int i = 0; i < y; i++) printf("\n");
        char msg1[128], msg2[128];
        snprintf(msg1, sizeof(msg1), "Terminal too small!");
        snprintf(msg2, sizeof(msg2), "Current: %s%dx%d%s  Required: %s%dx%d%s",
                 (term_width < MIN_TERM_WIDTH ? BRED : BGREEN), term_width, term_height, RST,
                 CYAN, MIN_TERM_WIDTH, MIN_TERM_HEIGHT, RST);

        int pad1 = (term_width - (int)strlen(msg1)) / 2;
        int pad2 = (term_width - 40) / 2; /* approximate */
        if (pad1 < 0) pad1 = 0;
        if (pad2 < 0) pad2 = 0;
        printf("%*s" BOLD_RED "%s" RST "\n", pad1, "", msg1);
        printf("%*s%s\n", pad2, "", msg2);
        fflush(stdout);
        return;
    }

    /* Title box */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm_now);

    int title_inner = 40;
    int title_pad = (term_width - title_inner - 4) / 2;
    if (title_pad < 0) title_pad = 0;

    printf("\n");
    printf("%*s  ╔", title_pad, "");
    for (int i = 0; i < title_inner; i++) printf("═");
    printf("╗\n");

    printf("%*s  ║" BOLD BCYAN "   Remote System Monitor  Dashboard   " RST "║\n", title_pad, "");

    char timeline[64];
    snprintf(timeline, sizeof(timeline), "   %s", timestr);
    int tl_len = strlen(timeline);
    printf("%*s  ║%s", title_pad, "", timeline);
    for (int i = tl_len; i < title_inner; i++) printf(" ");
    printf("║\n");

    printf("%*s  ╚", title_pad, "");
    for (int i = 0; i < title_inner; i++) printf("═");
    printf("╝\n\n");

    /* Table header separator */
    #define HSEP(w) do { for (int _i = 0; _i < (w); _i++) printf("─"); } while(0)

    /* Top border */
    printf("┌"); HSEP(COL_HOST); printf("┬"); HSEP(COL_CPU); printf("┬");
    HSEP(COL_RAM); printf("┬"); HSEP(COL_DISK); printf("┬"); HSEP(COL_STATUS);
    printf("┬"); HSEP(COL_ALERT); printf("┐\n");

    /* Header row */
    printf("│" BOLD " %-*s" RST, COL_HOST - 1, "HOST");
    printf("│" BOLD " %-*s" RST, COL_CPU - 1, "CPU%");
    printf("│" BOLD " %-*s" RST, COL_RAM - 1, "RAM%");
    printf("│" BOLD " %-*s" RST, COL_DISK - 1, "DISK%");
    printf("│" BOLD " %-*s" RST, COL_STATUS - 1, "STATUS");
    printf("│" BOLD " %-*s" RST, COL_ALERT - 1, "LAST ALERT");
    printf("│\n");

    /* Header separator */
    printf("├"); HSEP(COL_HOST); printf("┼"); HSEP(COL_CPU); printf("┼");
    HSEP(COL_RAM); printf("┼"); HSEP(COL_DISK); printf("┼"); HSEP(COL_STATUS);
    printf("┼"); HSEP(COL_ALERT); printf("┤\n");

    /* Data rows */
    for (int i = 0; i < host_count; i++) {
        HostInfo *h = &hosts[i];

        /* Check offline timeout */
        if (h->online && (now - h->last_seen > OFFLINE_TIMEOUT)) {
            h->online = 0;
            h->fd = -1;
        }

        /* HOST */
        printf("│ %-*s", COL_HOST - 1, h->name);

        /* CPU progress bar */
        char pb[256];
        render_progress_bar(pb, sizeof(pb), h->cpu, 6);
        pad_to(pb, sizeof(pb), COL_CPU - 1);
        printf("│%s", pb);

        /* RAM progress bar */
        render_progress_bar(pb, sizeof(pb), h->ram, 6);
        pad_to(pb, sizeof(pb), COL_RAM - 1);
        printf("│%s", pb);

        /* DISK progress bar */
        render_progress_bar(pb, sizeof(pb), h->disk, 6);
        pad_to(pb, sizeof(pb), COL_DISK - 1);
        printf("│%s", pb);

        /* STATUS */
        if (h->online) {
            char status[128];
            snprintf(status, sizeof(status), " " BOLD_GRN "● ONLINE" RST);
            pad_to(status, sizeof(status), COL_STATUS);
            printf("│%s", status);
        } else {
            char status[128];
            snprintf(status, sizeof(status), " " DIM RED "○ OFFLINE" RST);
            pad_to(status, sizeof(status), COL_STATUS);
            printf("│%s", status);
        }

        /* ALERT */
        if (h->alert[0]) {
            char alert_cell[256];
            snprintf(alert_cell, sizeof(alert_cell), " " BOLD_RED "%.*s" RST,
                     COL_ALERT - 3, h->alert);
            pad_to(alert_cell, sizeof(alert_cell), COL_ALERT);
            printf("│%s", alert_cell);
        } else {
            printf("│ %-*s", COL_ALERT - 1, "");
        }
        printf("│\n");
    }

    /* If no hosts */
    if (host_count == 0) {
        int total_inner = COL_HOST + COL_CPU + COL_RAM + COL_DISK + COL_STATUS + COL_ALERT + 5;
        printf("│" DIM);
        char msg[] = " Waiting for agents to connect...";
        printf(" %-*s", total_inner - 1, msg);
        printf(RST "│\n");
    }

    /* Bottom border */
    printf("└"); HSEP(COL_HOST); printf("┴"); HSEP(COL_CPU); printf("┴");
    HSEP(COL_RAM); printf("┴"); HSEP(COL_DISK); printf("┴"); HSEP(COL_STATUS);
    printf("┴"); HSEP(COL_ALERT); printf("┘\n");

    /* Footer info */
    int viewer_count = 0;
    int agent_online = 0;
    for (int i = 0; i < client_count; i++) {
        if (clients[i].type == CLIENT_VIEWER) viewer_count++;
    }
    for (int i = 0; i < host_count; i++) {
        if (hosts[i].online) agent_online++;
    }
    printf(DIM " Agents: %d/%d online (%d total registered) │ Viewers: %d │ Use nc/telnet for /view /history /log /help" RST "\n",
           agent_online, host_count, MAX_AGENTS, viewer_count);

    fflush(stdout);
}

/* ─── Viewer Command Handling ─────────────────────────────────────── */

static void handle_viewer_command(ClientInfo *c, const char *cmd) {
    char response[BUF_SIZE * 4];
    int pos = 0;

    /* trim leading whitespace */
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    /* trim trailing whitespace/newline */
    char cmd_copy[LINE_BUF_LEN];
    strncpy(cmd_copy, cmd, LINE_BUF_LEN - 1);
    cmd_copy[LINE_BUF_LEN - 1] = '\0';
    int len = strlen(cmd_copy);
    while (len > 0 && (cmd_copy[len-1] == '\n' || cmd_copy[len-1] == '\r' || cmd_copy[len-1] == ' '))
        cmd_copy[--len] = '\0';

    if (strcmp(cmd_copy, "/help") == 0) {
        pos += snprintf(response + pos, sizeof(response) - pos,
            "\r\n=== Monitor Server Commands ===\r\n"
            "  /view              - Show all hosts and their current metrics/status\r\n"
            "  /history <host> <minutes> - View metric history (e.g., /history web-1 10)\r\n"
            "  /log [count]       - View recent connection events (default: 20)\r\n"
            "  /help              - Show this help message\r\n\r\n");
        send_str(c->fd, response);
    }
    else if (strcmp(cmd_copy, "/view") == 0) {
        pos += snprintf(response + pos, sizeof(response) - pos,
            "\r\n=== Connected Hosts ===\r\n");
        if (host_count == 0) {
            pos += snprintf(response + pos, sizeof(response) - pos,
                "  No hosts registered.\r\n\r\n");
        } else {
            pos += snprintf(response + pos, sizeof(response) - pos,
                "  %-20s %8s %8s %8s %10s\r\n",
                "HOST", "CPU%", "RAM%", "DISK%", "STATUS");
            pos += snprintf(response + pos, sizeof(response) - pos,
                "  %-20s %8s %8s %8s %10s\r\n",
                "----", "----", "----", "-----", "------");
            time_t now = time(NULL);
            for (int i = 0; i < host_count; i++) {
                const char *status = (hosts[i].online && (now - hosts[i].last_seen <= OFFLINE_TIMEOUT))
                                     ? "ONLINE" : "OFFLINE";
                pos += snprintf(response + pos, sizeof(response) - pos,
                    "  %-20s %7.1f%% %7.1f%% %7.1f%% %10s\r\n",
                    hosts[i].name, hosts[i].cpu, hosts[i].ram, hosts[i].disk, status);
            }
            pos += snprintf(response + pos, sizeof(response) - pos, "\r\n");
        }
        send_str(c->fd, response);
    }
    else if (strncmp(cmd_copy, "/history", 8) == 0) {
        char hname[NAME_LEN] = "";
        int minutes = 10;
        sscanf(cmd_copy + 8, " %63s %d", hname, &minutes);
        if (hname[0] == '\0') {
            send_str(c->fd, "Usage: /history <host> <minutes>\r\n");
            return;
        }
        int idx = find_host(hname);
        if (idx < 0) {
            snprintf(response, sizeof(response), "Host '%s' not found.\r\n", hname);
            send_str(c->fd, response);
            return;
        }
        time_t cutoff = time(NULL) - minutes * 60;
        HostInfo *h = &hosts[idx];

        pos = snprintf(response, sizeof(response),
            "\r\n=== History for '%s' (last %d min) ===\r\n"
            "  %-20s %8s %8s %8s\r\n"
            "  %-20s %8s %8s %8s\r\n",
            hname, minutes,
            "TIME", "CPU%", "RAM%", "DISK%",
            "----", "----", "----", "-----");

        int count = 0;
        for (int j = 0; j < h->hist_count; j++) {
            int real_idx = (h->hist_head - h->hist_count + j + HISTORY_MAXLEN) % HISTORY_MAXLEN;
            HistoryEntry *e = &h->history[real_idx];
            if (e->timestamp >= cutoff) {
                struct tm tm_e;
                localtime_r(&e->timestamp, &tm_e);
                char ts[32];
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_e);
                pos += snprintf(response + pos, sizeof(response) - pos,
                    "  %-20s %7.1f%% %7.1f%% %7.1f%%\r\n",
                    ts, e->cpu, e->ram, e->disk);
                count++;
                if (pos > (int)sizeof(response) - 200) break;
            }
        }
        if (count == 0) {
            pos += snprintf(response + pos, sizeof(response) - pos,
                "  No data in the requested time range.\r\n");
        }
        pos += snprintf(response + pos, sizeof(response) - pos, "\r\n");
        send_str(c->fd, response);
    }
    else if (strncmp(cmd_copy, "/log", 4) == 0) {
        int count = 20;
        sscanf(cmd_copy + 4, " %d", &count);
        if (count > event_count) count = event_count;
        if (count <= 0) count = event_count;

        pos = snprintf(response, sizeof(response),
            "\r\n=== Recent Events (last %d) ===\r\n", count);

        int start = event_count - count;
        if (start < 0) start = 0;
        for (int j = start; j < event_count; j++) {
            int real_idx;
            if (event_count <= EVENT_LOG_MAXLEN) {
                real_idx = j;
            } else {
                real_idx = (event_head - event_count + j + EVENT_LOG_MAXLEN) % EVENT_LOG_MAXLEN;
            }
            EventEntry *e = &event_log[real_idx];
            struct tm tm_e;
            localtime_r(&e->timestamp, &tm_e);
            char ts[32];
            strftime(ts, sizeof(ts), "%H:%M:%S", &tm_e);
            pos += snprintf(response + pos, sizeof(response) - pos,
                "  [%s] %s\r\n", ts, e->msg);
            if (pos > (int)sizeof(response) - 200) break;
        }
        pos += snprintf(response + pos, sizeof(response) - pos, "\r\n");
        send_str(c->fd, response);
    }
    else {
        snprintf(response, sizeof(response),
            "Unknown command: '%s'. Type /help for available commands.\r\n", cmd_copy);
        send_str(c->fd, response);
    }
}

/* ─── Process Agent Data ──────────────────────────────────────────── */

static void process_agent_line(ClientInfo *c, const char *line) {
    char hostname[NAME_LEN] = "";
    double cpu = 0, ram = 0, disk = 0;
    long timestamp = 0;

    if (json_get_string(line, "host", hostname, NAME_LEN) < 0) return;
    json_get_double(line, "cpu", &cpu);
    json_get_double(line, "ram", &ram);
    json_get_double(line, "disk", &disk);
    json_get_long(line, "timestamp", &timestamp);

    if (hostname[0] == '\0') return;

    /* First message from this client — classify it as agent */
    if (c->type == CLIENT_UNKNOWN) {
        /* Check if agent name already connected */
        int idx = find_host(hostname);
        if (idx >= 0 && hosts[idx].online) {
            /* Duplicate name with active agent */
            char err[256];
            snprintf(err, sizeof(err),
                "ERROR: Agent name '%s' is already connected. Choose a different --name.\n",
                hostname);
            send_str(c->fd, err);
            add_event("Rejected agent '%s' from %s (duplicate name)", hostname, c->addr);
            return; /* caller will see type is still UNKNOWN and can clean up if needed */
        }

        if (idx < 0) {
            /* New host — check capacity */
            if (host_count >= MAX_AGENTS) {
                char err[256];
                snprintf(err, sizeof(err),
                    "ERROR: Server full (%d agents max). Connection rejected.\n", MAX_AGENTS);
                send_str(c->fd, err);
                add_event("Rejected agent '%s' from %s (server full)", hostname, c->addr);
                return;
            }
            idx = add_host(hostname);
        }

        c->type = CLIENT_AGENT;
        c->host_idx = idx;
        hosts[idx].online = 1;
        hosts[idx].fd = c->fd;
        add_event("Agent '%s' connected from %s", hostname, c->addr);
    }

    int idx = c->host_idx;
    if (idx < 0 || idx >= host_count) return;

    hosts[idx].cpu  = cpu;
    hosts[idx].ram  = ram;
    hosts[idx].disk = disk;
    hosts[idx].last_seen = (timestamp > 0) ? (time_t)timestamp : time(NULL);
    hosts[idx].online = 1;

    add_history(idx, cpu, ram, disk, hosts[idx].last_seen);
    check_alerts(idx);
    g_need_redraw = 1;
}

/* ─── Process line from client ────────────────────────────────────── */

static void process_client_line(ClientInfo *c, const char *line) {
    /* Detect if this is a JSON message (agent) or a command (viewer) */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '{') {
        /* JSON — agent data */
        process_agent_line(c, line);
    } else if (*p == '/') {
        /* Command — viewer */
        if (c->type == CLIENT_UNKNOWN) {
            c->type = CLIENT_VIEWER;
            add_event("Viewer connected from %s", c->addr);
        }
        handle_viewer_command(c, p);
    } else if (*p && *p != '\n' && *p != '\r') {
        /* Unknown text — treat as viewer */
        if (c->type == CLIENT_UNKNOWN) {
            c->type = CLIENT_VIEWER;
            add_event("Viewer connected from %s", c->addr);
        }
        handle_viewer_command(c, p);
    }
}

/* ─── Main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    const char *bind_host = "0.0.0.0";
    const char *config_file = "thresholds.conf";

    /* Parse CLI */
    static struct option long_opts[] = {
        {"port",   required_argument, NULL, 'p'},
        {"host",   required_argument, NULL, 'h'},
        {"config", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:h:c:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'h': bind_host = optarg; break;
            case 'c': config_file = optarg; break;
            default:
                fprintf(stderr, "Usage: %s [--port PORT] [--host HOST] [--config FILE]\n", argv[0]);
                return 1;
        }
    }

    /* Load config */
    parse_config(config_file);

    /* Signals */
    signal(SIGWINCH, sigwinch_handler);
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Create listening socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_host, &addr.sin_addr);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, 10) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }

    set_nonblocking(listen_fd);
    setup_terminal();
    atexit(restore_terminal);

    add_event("Server started on %s:%d", bind_host, port);

    /* Main event loop */
    time_t last_draw = 0;

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        for (int i = 0; i < client_count; i++) {
            FD_SET(clients[i].fd, &rfds);
            if (clients[i].fd > maxfd) maxfd = clients[i].fd;
        }

        struct timeval tv = {1, 0}; /* 1 second timeout for periodic redraw */
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (ready < 0) {
            if (errno == EINTR) {
                /* signal interrupted, just redraw */
                if (g_need_redraw) {
                    draw_dashboard();
                    g_need_redraw = 0;
                    last_draw = time(NULL);
                }
                continue;
            }
            break;
        }

        /* Accept new connections */
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int cli_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
            if (cli_fd >= 0) {
                set_nonblocking(cli_fd);
                char addr_str[64];
                snprintf(addr_str, sizeof(addr_str), "%s:%d",
                         inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));
                ClientInfo *c = add_client(cli_fd, addr_str);
                if (!c) {
                    send_str(cli_fd, "ERROR: Too many connections.\n");
                    close(cli_fd);
                }
            }
        }

        /* Read data from clients */
        for (int i = 0; i < client_count; i++) {
            if (!FD_ISSET(clients[i].fd, &rfds)) continue;

            char buf[BUF_SIZE];
            int n = recv(clients[i].fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                /* Client disconnected */
                remove_client(i);
                i--;
                g_need_redraw = 1;
                continue;
            }
            buf[n] = '\0';

            /* Append to line buffer and process complete lines */
            ClientInfo *c = &clients[i];
            for (int j = 0; j < n; j++) {
                if (buf[j] == '\n') {
                    c->line_buf[c->line_len] = '\0';
                    if (c->line_len > 0) {
                        process_client_line(c, c->line_buf);
                        /* Check if client was rejected (type still UNKNOWN after processing JSON) */
                        /* The client might have been removed during processing, but since we
                           process in place, we just continue */
                    }
                    c->line_len = 0;
                } else if (c->line_len < LINE_BUF_LEN - 1) {
                    c->line_buf[c->line_len++] = buf[j];
                }
            }
        }

        /* Periodic or signaled redraw */
        time_t now = time(NULL);
        if (g_need_redraw || (now - last_draw >= REFRESH_INTERVAL)) {
            draw_dashboard();
            g_need_redraw = 0;
            last_draw = now;
        }
    }

    /* Cleanup */
    for (int i = 0; i < client_count; i++) {
        close(clients[i].fd);
    }
    close(listen_fd);
    for (int i = 0; i < host_count; i++) {
        free(hosts[i].history);
    }

    return 0;
}
