/*
 * modbus_sniffer.c — Passive RS485 Modbus RTU sniffer for EG4 LifePower4 batteries
 *
 * Reads the RS485 bus between an EG4 3000EHV-48 inverter and LifePower4
 * batteries, decodes battery register responses, and writes a JSON file
 * with the latest readings. POSIX-only, no external dependencies.
 *
 * Usage: modbus_sniffer -s /dev/ttyUSB0 -o /tmp/battery_data.json [-b 9600] [-d] [-p pidfile]
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------- constants ---------- */

#define BUF_SIZE      4096
#define FRAME_LEN     39      /* slave(1) + func(1) + bytecnt(1) + data(34) + crc(2) */
#define NUM_REGS      17
#define MAX_BATTERIES 16
#define SIGNATURE_FUNC  0x03
#define SIGNATURE_BCNT  0x22  /* 34 bytes = 17 registers */

/* ---------- globals ---------- */

static volatile sig_atomic_t g_running = 1;
static int   g_daemon    = 0;
static char *g_pidfile   = NULL;
static char *g_serial    = NULL;
static char *g_outfile   = NULL;
static int   g_baud      = 9600;

/* per-battery decoded data */
typedef struct {
    int      valid;
    time_t   timestamp;
    uint8_t  slave_id;
    uint16_t regs[NUM_REGS];
    /* decoded */
    int      soc_pct;
    double   voltage_v;
    double   current_a;
    int      temperature_c;
    int      cycle_count;
    double   max_charge_current_a;
    double   max_discharge_current_a;
    int      soh_pct;
    double   max_charge_voltage_v;
} battery_t;

static battery_t g_batteries[MAX_BATTERIES];

/* ---------- logging ---------- */

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_daemon) {
        vsyslog(LOG_INFO, fmt, ap);
    } else {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

/* ---------- signal handling ---------- */

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/* ---------- Modbus CRC16 ---------- */

static uint16_t crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

/* ---------- serial port ---------- */

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return B9600;
    }
}

static int open_serial(const char *path, int baud)
{
    int fd = open(path, O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        logmsg("open(%s): %s", path, strerror(errno));
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        logmsg("tcgetattr: %s", strerror(errno));
        close(fd);
        return -1;
    }

    speed_t sp = baud_to_speed(baud);
    cfsetispeed(&tty, sp);
    cfsetospeed(&tty, sp);

    /* 8N1, no flow control, read-only */
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |= CLOCAL | CREAD;

    /* raw mode */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                      INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    tty.c_cc[VMIN]  = 1;   /* block until at least 1 byte */
    tty.c_cc[VTIME] = 1;   /* 100ms inter-byte timeout */

    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        logmsg("tcsetattr: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

/* ---------- frame decoder ---------- */

static void decode_frame(const uint8_t *frame)
{
    uint8_t slave_id = frame[0];
    if (slave_id == 0 || slave_id > MAX_BATTERIES) {
        logmsg("ignoring frame with slave_id=%d", slave_id);
        return;
    }

    battery_t *b = &g_batteries[slave_id - 1];

    /* extract 17 registers (big-endian) */
    for (int i = 0; i < NUM_REGS; i++) {
        b->regs[i] = (uint16_t)((frame[3 + i * 2] << 8) | frame[3 + i * 2 + 1]);
    }

    /* decode confirmed fields */
    b->soc_pct       = (int)b->regs[2];                               /* reg 21 */
    b->voltage_v     = b->regs[3] / 100.0;                            /* reg 22 */
    int16_t raw_cur  = (int16_t)b->regs[4];
    b->current_a     = raw_cur / 100.0;                                /* reg 23 */
    b->temperature_c = (int)b->regs[5];                                /* reg 24 */

    /* high-confidence inferred fields */
    b->cycle_count           = (int)b->regs[0];                        /* reg 19 */
    b->max_charge_current_a  = b->regs[7] / 1000.0;                   /* reg 26 */
    b->max_discharge_current_a = b->regs[8] / 1000.0;                 /* reg 27 */
    b->soh_pct               = (int)b->regs[13];                       /* reg 32 */
    b->max_charge_voltage_v  = b->regs[14] / 100.0;                   /* reg 33 */

    b->slave_id  = slave_id;
    b->timestamp = time(NULL);
    b->valid     = 1;

    logmsg("[slave %d] SOC=%d%% V=%.2f I=%.2f T=%d°C cycles=%d",
           slave_id, b->soc_pct, b->voltage_v, b->current_a,
           b->temperature_c, b->cycle_count);
}

/* ---------- buffer scanner ---------- */

static uint8_t g_buf[BUF_SIZE];
static size_t  g_buflen = 0;

static void process_buffer(void)
{
    while (g_buflen >= FRAME_LEN) {
        /* scan for [XX] 0x03 0x22 where XX is any non-zero slave ID */
        size_t idx;
        int found = 0;
        for (idx = 0; idx + FRAME_LEN <= g_buflen; idx++) {
            if (g_buf[idx] != 0x00 &&
                g_buf[idx + 1] == SIGNATURE_FUNC &&
                g_buf[idx + 2] == SIGNATURE_BCNT) {
                found = 1;
                break;
            }
        }

        if (!found) {
            /* keep tail that might be start of a frame */
            if (g_buflen > FRAME_LEN) {
                size_t keep = FRAME_LEN - 1;
                memmove(g_buf, g_buf + g_buflen - keep, keep);
                g_buflen = keep;
            }
            return;
        }

        /* discard bytes before the match */
        if (idx > 0) {
            memmove(g_buf, g_buf + idx, g_buflen - idx);
            g_buflen -= idx;
        }

        if (g_buflen < FRAME_LEN)
            return;

        /* verify CRC */
        uint16_t crc_recv = (uint16_t)(g_buf[37] | (g_buf[38] << 8));
        uint16_t crc_calc = crc16(g_buf, 37);

        if (crc_recv == crc_calc) {
            decode_frame(g_buf);
        }

        /* advance past this frame */
        memmove(g_buf, g_buf + FRAME_LEN, g_buflen - FRAME_LEN);
        g_buflen -= FRAME_LEN;
    }
}

/* ---------- JSON writer ---------- */

static void iso8601(time_t t, char *buf, size_t len)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int write_json(const char *path)
{
    /* build temp path: <path>.tmp */
    size_t plen = strlen(path);
    char *tmp = malloc(plen + 5);
    if (!tmp) return -1;
    snprintf(tmp, plen + 5, "%s.tmp", path);

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        logmsg("fopen(%s): %s", tmp, strerror(errno));
        free(tmp);
        return -1;
    }

    char tsbuf[32];
    iso8601(time(NULL), tsbuf, sizeof(tsbuf));

    fprintf(fp, "{\n  \"updated\": \"%s\",\n  \"batteries\": {", tsbuf);

    int first = 1;
    for (int i = 0; i < MAX_BATTERIES; i++) {
        battery_t *b = &g_batteries[i];
        if (!b->valid) continue;

        char bts[32];
        iso8601(b->timestamp, bts, sizeof(bts));

        if (!first) fprintf(fp, ",");
        first = 0;

        fprintf(fp, "\n    \"%d\": {\n", b->slave_id);
        fprintf(fp, "      \"timestamp\": \"%s\",\n", bts);
        fprintf(fp, "      \"slave_id\": %d,\n", b->slave_id);
        fprintf(fp, "      \"soc_pct\": %d,\n", b->soc_pct);
        fprintf(fp, "      \"voltage_v\": %.2f,\n", b->voltage_v);
        fprintf(fp, "      \"current_a\": %.2f,\n", b->current_a);
        fprintf(fp, "      \"temperature_c\": %d,\n", b->temperature_c);
        fprintf(fp, "      \"cycle_count\": %d,\n", b->cycle_count);
        fprintf(fp, "      \"max_charge_current_a\": %.1f,\n", b->max_charge_current_a);
        fprintf(fp, "      \"max_discharge_current_a\": %.1f,\n", b->max_discharge_current_a);
        fprintf(fp, "      \"soh_pct\": %d,\n", b->soh_pct);
        fprintf(fp, "      \"max_charge_voltage_v\": %.2f,\n", b->max_charge_voltage_v);
        fprintf(fp, "      \"raw_registers\": [");
        for (int r = 0; r < NUM_REGS; r++) {
            fprintf(fp, "%s%u", r ? ", " : "", b->regs[r]);
        }
        fprintf(fp, "]\n    }");
    }

    fprintf(fp, "\n  }\n}\n");
    fclose(fp);

    /* atomic rename */
    if (rename(tmp, path) != 0) {
        logmsg("rename(%s, %s): %s", tmp, path, strerror(errno));
        unlink(tmp);
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

/* ---------- daemonize ---------- */

static void daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0)
        exit(EXIT_SUCCESS);  /* parent exits */

    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* redirect stdin/stdout/stderr to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }

    openlog("modbus_sniffer", LOG_PID, LOG_DAEMON);
}

static void write_pidfile(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%d\n", (int)getpid());
        fclose(fp);
    }
}

static void remove_pidfile(void)
{
    if (g_pidfile)
        unlink(g_pidfile);
}

/* ---------- usage ---------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -s <serial_port> -o <json_output> [-b baud] [-d] [-p pidfile]\n"
        "\n"
        "  -s PORT    Serial port (e.g. /dev/ttyUSB0)\n"
        "  -o FILE    JSON output file (e.g. /tmp/battery_data.json)\n"
        "  -b BAUD    Baud rate (default: 9600)\n"
        "  -d         Daemonize (fork to background, log to syslog)\n"
        "  -p FILE    PID file (default: /var/run/modbus_sniffer.pid)\n",
        prog);
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt(argc, argv, "s:o:b:dp:h")) != -1) {
        switch (opt) {
        case 's': g_serial  = optarg; break;
        case 'o': g_outfile = optarg; break;
        case 'b': g_baud    = atoi(optarg); break;
        case 'd': g_daemon  = 1; break;
        case 'p': g_pidfile = optarg; break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (!g_serial || !g_outfile) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!g_pidfile)
        g_pidfile = "/var/run/modbus_sniffer.pid";

    install_signals();

    if (g_daemon)
        daemonize();

    write_pidfile(g_pidfile);
    atexit(remove_pidfile);

    logmsg("starting: port=%s baud=%d output=%s", g_serial, g_baud, g_outfile);

    int fd = open_serial(g_serial, g_baud);
    if (fd < 0) {
        logmsg("failed to open serial port");
        return EXIT_FAILURE;
    }

    time_t last_write = 0;

    while (g_running) {
        uint8_t tmp[256];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            logmsg("read: %s", strerror(errno));
            break;
        }
        if (n == 0) continue;

        /* append to buffer */
        size_t avail = BUF_SIZE - g_buflen;
        if ((size_t)n > avail) {
            /* overflow — discard oldest */
            size_t discard = (size_t)n - avail;
            memmove(g_buf, g_buf + discard, g_buflen - discard);
            g_buflen -= discard;
        }
        memcpy(g_buf + g_buflen, tmp, (size_t)n);
        g_buflen += (size_t)n;

        process_buffer();

        /* write JSON at most once per second */
        time_t now = time(NULL);
        if (now != last_write) {
            int any_valid = 0;
            for (int i = 0; i < MAX_BATTERIES; i++) {
                if (g_batteries[i].valid) { any_valid = 1; break; }
            }
            if (any_valid) {
                write_json(g_outfile);
                last_write = now;
            }
        }
    }

    close(fd);
    logmsg("shutting down");

    if (g_daemon)
        closelog();

    return EXIT_SUCCESS;
}
