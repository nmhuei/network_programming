/*
 * agent.c — Remote System Monitor Agent
 *
 * Thu thập CPU/RAM/Disk từ /proc (Linux) và gửi JSON qua TCP đến server.
 * Auto-reconnect khi mất kết nối, hiển thị bảng metrics trên terminal.
 *
 * Build:  gcc -Wall -o agent agent.c -lm
 * Usage:  ./agent --server HOST:PORT [--interval SECONDS] [--name HOSTNAME]
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <termios.h>

/* ─── Constants ───────────────────────────────────────────────────── */
#define RECONNECT_DELAY  5
#define MAX_RETRIES      5
#define BUF_SIZE         4096
#define NAME_LEN         64

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
#define HIDE_CUR  "\033[?25l"
#define SHOW_CUR  "\033[?25h"

/* ─── Global State ────────────────────────────────────────────────── */
static volatile sig_atomic_t g_running = 1;
static struct termios orig_termios;
static int termios_saved = 0;
static int header_printed = 0;

/* ─── Signal Handlers ─────────────────────────────────────────────── */

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

/* ─── Metric Collection ──────────────────────────────────────────── */

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} CpuSample;

static int read_cpu_sample(CpuSample *s) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[256];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    /* cpu  user nice system idle iowait irq softirq steal */
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &s->user, &s->nice, &s->system, &s->idle,
               &s->iowait, &s->irq, &s->softirq, &s->steal) < 4) {
        return -1;
    }
    return 0;
}

static double get_cpu_percent(void) {
    CpuSample s1, s2;
    if (read_cpu_sample(&s1) < 0) return -1.0;
    usleep(500000); /* 0.5s */
    if (read_cpu_sample(&s2) < 0) return -1.0;

    unsigned long long total1 = s1.user + s1.nice + s1.system + s1.idle +
                                s1.iowait + s1.irq + s1.softirq + s1.steal;
    unsigned long long total2 = s2.user + s2.nice + s2.system + s2.idle +
                                s2.iowait + s2.irq + s2.softirq + s2.steal;

    unsigned long long idle1 = s1.idle + s1.iowait;
    unsigned long long idle2 = s2.idle + s2.iowait;

    unsigned long long total_d = total2 - total1;
    unsigned long long idle_d  = idle2 - idle1;

    if (total_d == 0) return 0.0;
    return (double)(total_d - idle_d) / (double)total_d * 100.0;
}

static double get_ram_percent(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1.0;

    unsigned long long mem_total = 0, mem_available = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, " %llu", &mem_total);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, " %llu", &mem_available);
        }
        if (mem_total && mem_available) break;
    }
    fclose(f);

    if (mem_total == 0) return -1.0;
    return (double)(mem_total - mem_available) / (double)mem_total * 100.0;
}

static double get_disk_percent(void) {
    struct statvfs st;
    if (statvfs("/", &st) < 0) return -1.0;
    if (st.f_blocks == 0) return -1.0;
    return (double)(st.f_blocks - st.f_bfree) / (double)st.f_blocks * 100.0;
}

/* ─── Color Helpers ───────────────────────────────────────────────── */

static const char* color_for_val(double val) {
    if (val >= 85.0) return BRED;
    if (val >= 70.0) return BYELLOW;
    return BGREEN;
}

/* ─── Terminal Output ─────────────────────────────────────────────── */

static void print_header(void) {
    printf(BOLD "+---------------------+--------+--------+--------+" RST "\n");
    printf(BOLD "| TIME                | CPU%%   | RAM%%   | DISK%%  |" RST "\n");
    printf(BOLD "+---------------------+--------+--------+--------+" RST "\n");
    fflush(stdout);
    header_printed = 1;
}

static void print_metric_row(double cpu, double ram, double disk) {
    if (!header_printed) print_header();

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timestr[32];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", &tm_now);

    printf("| %s | %s%5.1f%s  | %s%5.1f%s  | %s%5.1f%s  |\n",
           timestr,
           color_for_val(cpu), cpu, RST,
           color_for_val(ram), ram, RST,
           color_for_val(disk), disk, RST);
    fflush(stdout);
}

/* ─── TCP Connection ──────────────────────────────────────────────── */

static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    /* Try to resolve hostname */
    struct hostent *he = gethostbyname(host);
    if (he) {
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    } else if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/* Check if server sent an error/rejection message */
static int check_server_response(int fd) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = {0, 100000}; /* 100ms */

    if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0) {
        char buf[BUF_SIZE];
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            /* Check for ERROR: prefix */
            if (strncmp(buf, "ERROR:", 6) == 0) {
                /* Trim trailing newline for clean display */
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
                    buf[--n] = '\0';
                fprintf(stderr, BOLD_RED "%s" RST "\n", buf);
                return -1; /* fatal error, should exit */
            }
        } else if (n == 0) {
            return -2; /* server closed connection */
        }
    }
    return 0; /* no error */
}

/* ─── Build JSON Frame ────────────────────────────────────────────── */

static int build_json(char *buf, int buflen, const char *host,
                      double cpu, double ram, double disk) {
    time_t now = time(NULL);
    return snprintf(buf, buflen,
        "{\"host\":\"%s\",\"timestamp\":%ld,\"cpu\":%.1f,\"ram\":%.1f,\"disk\":%.1f}\n",
        host, (long)now, cpu, ram, disk);
}

/* ─── Main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    char server_host[256] = "";
    int  server_port = 0;
    double interval = 5.0;
    char agent_name[NAME_LEN] = "";

    /* Parse CLI */
    static struct option long_opts[] = {
        {"server",   required_argument, NULL, 's'},
        {"interval", required_argument, NULL, 'i'},
        {"name",     required_argument, NULL, 'n'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:i:n:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 's': {
                /* Parse HOST:PORT */
                char *colon = strrchr(optarg, ':');
                if (colon) {
                    *colon = '\0';
                    strncpy(server_host, optarg, sizeof(server_host) - 1);
                    server_port = atoi(colon + 1);
                } else {
                    strncpy(server_host, optarg, sizeof(server_host) - 1);
                    server_port = 8784; /* default */
                }
                break;
            }
            case 'i': interval = atof(optarg); break;
            case 'n': strncpy(agent_name, optarg, NAME_LEN - 1); break;
            default:
                fprintf(stderr, "Usage: %s --server HOST:PORT [--interval SECONDS] [--name HOSTNAME]\n", argv[0]);
                return 1;
        }
    }

    if (server_host[0] == '\0' || server_port <= 0) {
        fprintf(stderr, "Error: --server HOST:PORT is required\n");
        fprintf(stderr, "Usage: %s --server HOST:PORT [--interval SECONDS] [--name HOSTNAME]\n", argv[0]);
        return 1;
    }

    /* Default name = system hostname */
    if (agent_name[0] == '\0') {
        gethostname(agent_name, NAME_LEN - 1);
    }

    if (interval < 1.0) interval = 1.0;

    /* Signals */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    setup_terminal();
    atexit(restore_terminal);

    printf(BCYAN "[agent]" RST " Starting agent '" BOLD "%s" RST "'  interval=%.1fs\n", agent_name, interval);
    fflush(stdout);

    int retries = 0;

    while (g_running) {
        /* Connect */
        printf(DIM "[agent] Connecting to %s:%d..." RST "\n", server_host, server_port);
        fflush(stdout);

        int fd = tcp_connect(server_host, server_port);
        if (fd < 0) {
            retries++;
            if (retries > MAX_RETRIES) {
                fprintf(stderr, BOLD_RED "[agent] Max retries (%d) reached. Exiting." RST "\n", MAX_RETRIES);
                break;
            }
            fprintf(stderr, BYELLOW "[agent] Connection failed (attempt %d/%d). Retrying in %ds..." RST "\n",
                    retries, MAX_RETRIES, RECONNECT_DELAY);
            sleep(RECONNECT_DELAY);
            continue;
        }

        retries = 0; /* reset on successful connect */
        printf(BGREEN "[agent] Connected to %s:%d" RST "\n", server_host, server_port);
        fflush(stdout);

        header_printed = 0; /* reprint header on reconnect */

        /* Metric collection & send loop */
        while (g_running) {
            double cpu  = get_cpu_percent();
            double ram  = get_ram_percent();
            double disk = get_disk_percent();

            if (cpu < 0) cpu = 0;
            if (ram < 0) ram = 0;
            if (disk < 0) disk = 0;

            /* Build and send JSON */
            char json[BUF_SIZE];
            int len = build_json(json, sizeof(json), agent_name, cpu, ram, disk);

            int sent = send(fd, json, len, MSG_NOSIGNAL);
            if (sent <= 0) {
                fprintf(stderr, BYELLOW "[agent] Connection lost. Reconnecting..." RST "\n");
                break; /* reconnect */
            }

            /* Display on terminal */
            print_metric_row(cpu, ram, disk);

            /* Check for server error response */
            int srv_resp = check_server_response(fd);
            if (srv_resp == -1) {
                /* Fatal error from server (name conflict, full, etc.) */
                close(fd);
                return 1;
            } else if (srv_resp == -2) {
                /* Server closed */
                fprintf(stderr, BYELLOW "[agent] Server closed connection. Reconnecting..." RST "\n");
                break;
            }

            /* Wait for next interval (subtract the ~0.5s CPU measurement time) */
            double wait_time = interval - 0.5;
            if (wait_time < 0.5) wait_time = 0.5;

            /* Sleep in small increments so we can respond to SIGINT quickly */
            double slept = 0;
            while (slept < wait_time && g_running) {
                usleep(200000); /* 200ms */
                slept += 0.2;
            }
        }

        close(fd);

        if (g_running) {
            /* Wait before reconnect */
            printf(DIM "[agent] Waiting %ds before reconnect..." RST "\n", RECONNECT_DELAY);
            fflush(stdout);
            sleep(RECONNECT_DELAY);
            retries++;
            if (retries > MAX_RETRIES) {
                fprintf(stderr, BOLD_RED "[agent] Max retries (%d) reached. Exiting." RST "\n", MAX_RETRIES);
                break;
            }
        }
    }

    printf(DIM "[agent] Agent '%s' stopped." RST "\n", agent_name);
    return 0;
}
