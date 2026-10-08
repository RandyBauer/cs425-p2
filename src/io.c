/*
 * Layer 3: I/O. The command line, the socket, the relay hello, the poll loop, the clock and
 * the files. This is the only layer that knows any of them exist, and it is kept thin: wait
 * for an event, hand it to a state machine from layer 2, carry out what comes back.
 */
#include "lab.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- command line */

/* Writes a formatted one-line reason into err, if there is somewhere to write it. */
__attribute__((format(printf, 3, 4)))
static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || cap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* Parses a whole decimal integer from lo to hi. Rejects empty text and trailing junk,
 * which atoi would quietly turn into 0. */
static bool parse_number(const char *text, long lo, long hi, long *value)
{
    char *end = NULL;

    errno = 0;
    long v = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *value = v;
    return true;
}

/* The relay's session rule: 1 to 32 characters from a-z, 0-9 and -. */
static bool valid_session(const char *s)
{
    size_t n = strlen(s);

    if (n < 1 || n > 32) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* The relay's rate rule, [0-9]*\.?[0-9]+ and a value from 0 to 0.5. Checking it here turns
 * a rate the relay would refuse (a sign, an exponent like 1e-05) into a command-line error
 * with exit 1, and lets the text go into the hello exactly as typed. */
static bool valid_rate(const char *text)
{
    size_t digits_since_point = 0;
    bool point = false;

    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '.' && !point) {
            point = true;
            digits_since_point = 0;
        } else if (*p >= '0' && *p <= '9') {
            digits_since_point++;
        } else {
            return false;
        }
    }
    /* Needs a digit after the point, or at least one digit if there is no point. */
    return digits_since_point > 0 && strtod(text, NULL) <= 0.5;
}

int rdt_parse_args(int argc, char *argv[], struct rdt_config *cfg, char *err, size_t errcap)
{
    long value = 0;
    int opt;

    if (argc < 2 || argv == NULL || cfg == NULL) {
        set_err(err, errcap, "missing mode: expected send or recv");
        return -1;
    }
    memset(cfg, 0, sizeof *cfg);
    cfg->window = RDT_DEFAULT_WINDOW;
    cfg->timeout_ms = RDT_DEFAULT_TIMEOUT_MS;
    cfg->loss = "0";
    cfg->corrupt = "0";
    cfg->dup = "0";
    cfg->port = RDT_DEFAULT_PORT;

    if (strcmp(argv[1], "send") == 0) {
        cfg->mode = RDT_MODE_SEND;
    } else if (strcmp(argv[1], "recv") == 0) {
        cfg->mode = RDT_MODE_RECV;
    } else {
        set_err(err, errcap, "unknown mode '%s': expected send or recv", argv[1]);
        return -1;
    }

    /* getopt keeps its state in globals. On glibc (Onyx, Codespaces and CI all use it),
     * optind = 0 forces a full restart, so this can run more than once per process, as the
     * tests do. opterr = 0 because the messages below replace getopt's own. Passing
     * argv + 1 puts the mode where getopt expects the program name. */
    optind = 0;
    opterr = 0;
    while ((opt = getopt(argc - 1, argv + 1, ":s:w:T:l:c:d:p:")) != -1) {
        if (cfg->mode == RDT_MODE_RECV && strchr("wTlcd", opt) != NULL) {
            set_err(err, errcap, "-%c is only for send", opt);
            return -1;
        }
        switch (opt) {
        case 's':
            cfg->session = optarg;
            break;
        case 'w':
            if (!parse_number(optarg, 1, RDT_MAX_WINDOW, &value)) {
                set_err(err, errcap, "-w must be a whole number from 1 to %u", RDT_MAX_WINDOW);
                return -1;
            }
            cfg->window = (uint32_t)value; /* range-checked just above */
            break;
        case 'T':
            if (!parse_number(optarg, 1, RDT_MAX_TIMEOUT_MS, &value)) {
                set_err(err, errcap, "-T must be milliseconds from 1 to %u", RDT_MAX_TIMEOUT_MS);
                return -1;
            }
            cfg->timeout_ms = (uint32_t)value; /* range-checked just above */
            break;
        case 'l':
        case 'c':
        case 'd':
            if (!valid_rate(optarg)) {
                set_err(err, errcap, "-%c must be a decimal from 0 to 0.5, such as 0.05", opt);
                return -1;
            }
            if (opt == 'l') {
                cfg->loss = optarg;
            } else if (opt == 'c') {
                cfg->corrupt = optarg;
            } else {
                cfg->dup = optarg;
            }
            break;
        case 'p':
            if (!parse_number(optarg, 1, 65535, &value)) {
                set_err(err, errcap, "-p must be a port number from 1 to 65535");
                return -1;
            }
            cfg->port = optarg;
            break;
        case ':':
            set_err(err, errcap, "-%c needs a value", optopt);
            return -1;
        default:
            set_err(err, errcap, "unknown option -%c", optopt);
            return -1;
        }
    }

    if (argc - 1 - optind != 2) {
        set_err(err, errcap, "expected <relay> and <file> after the options");
        return -1;
    }
    cfg->relay = argv[1 + optind];
    cfg->file = argv[2 + optind];
    if (cfg->session == NULL) {
        set_err(err, errcap, "-s <session> is required");
        return -1;
    }
    if (!valid_session(cfg->session)) {
        set_err(err, errcap, "session must be 1 to 32 characters from a-z, 0-9 and -");
        return -1;
    }
    return 0;
}

void rdt_usage(FILE *f)
{
    if (f == NULL) {
        return;
    }
    fputs("Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n"
          "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
          "       myapp recv -s <session> [-p port] <relay> <file>\n"
          "\n"
          "  -s <session>     session name shared by the sender and the receiver\n"
          "  -w <window>      Go-Back-N window size in packets, 1 to 64 (default: 8)\n"
          "  -T <timeout-ms>  retransmission timeout in milliseconds (default: 250)\n"
          "  -l <loss>        probability the relay drops a packet (default: 0)\n"
          "  -c <corrupt>     probability the relay flips a bit (default: 0)\n"
          "  -d <dup>         probability the relay duplicates a packet (default: 0)\n"
          "  -p <port>        relay port (default: 4250)\n"
          "  <relay>          host name or address of the relay\n"
          "  <file>           file to send, or file to write what is received\n",
          f);
}

int rdt_hello_text(const struct rdt_config *cfg, char *buf, size_t cap)
{
    int n;

    if (cfg == NULL || buf == NULL || cfg->session == NULL
        || (cfg->mode == RDT_MODE_SEND
            && (cfg->loss == NULL || cfg->corrupt == NULL || cfg->dup == NULL))) {
        return -1;
    }
    if (cfg->mode == RDT_MODE_SEND) {
        n = snprintf(buf, cap, "HELLO %s send %s %s %s", cfg->session, cfg->loss, cfg->corrupt,
                     cfg->dup);
    } else {
        n = snprintf(buf, cap, "HELLO %s recv", cfg->session);
    }
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}

/* ----------------------------------------------------------------------------- clock */

uint64_t rdt_now_ms(void)
{
    struct timespec ts = { 0, 0 };

    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* Monotonic time is never negative, so both conversions are safe. */
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int rdt_poll_timeout(bool timer_set, uint64_t due_ms, uint64_t now_ms)
{
    if (!timer_set) {
        return -1;
    }
    /* A deadline already past must become 0, not a negative number: poll reads any
     * negative timeout as "wait forever", and the sender would hang with its timer expired. */
    if (due_ms <= now_ms) {
        return 0;
    }
    uint64_t left = due_ms - now_ms;
    return left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
}

/* ---------------------------------------------------------------------------- socket */

int rdt_open(const char *host, const char *port, char *err, size_t errcap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int fd = -1;

    if (host == NULL || port == NULL) {
        set_err(err, errcap, "missing relay host or port");
        return -1;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* the relay is IPv4 only; see lab.h */
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        set_err(err, errcap, "cannot resolve relay %s port %s: %s", host, port, gai_strerror(rc));
        return -1;
    }
    for (struct addrinfo *ai = res; ai != NULL && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        /* Excluded from coverage: branches on socket and connect failing. Both are library
         * calls a test cannot make fail, since connecting a UDP socket sends nothing. */
        if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) { // GCOVR_EXCL_START
            close(fd);
            fd = -1;
        } // GCOVR_EXCL_STOP
    }
    freeaddrinfo(res);
    if (fd < 0) { // GCOVR_EXCL_START
        set_err(err, errcap, "cannot open a UDP socket to %s port %s: %s", host, port,
                strerror(errno));
    } // GCOVR_EXCL_STOP
    return fd;
}

int rdt_wait(int fd, int timeout_ms, uint8_t *buf, size_t cap, size_t *len)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };

    if (buf == NULL || len == NULL) {
        return -1;
    }
    int rc = poll(&pfd, 1, timeout_ms);
    /* Excluded from coverage: poll itself failing (interrupted by a signal, or out of
     * memory), a library call a test cannot make fail. */
    if (rc < 0) { // GCOVR_EXCL_START
        return errno == EINTR ? 0 : -1;
    } // GCOVR_EXCL_STOP
    if (rc == 0) {
        return 0;
    }
    ssize_t n = recv(fd, buf, cap, 0);
    if (n < 0) {
        /* ECONNREFUSED is the ICMP "port unreachable" an earlier datagram earned, delivered
         * to a connected socket: nobody is listening. That is silence, not a broken socket. */
        return (errno == ECONNREFUSED || errno == EINTR) ? 0 : -1;
    }
    *len = (size_t)n; /* n >= 0 here */
    return 1;
}

int rdt_send_actions(int fd, const struct rdt_actions *act)
{
    if (act == NULL) {
        return -1;
    }
    for (size_t i = 0; i < act->ndgram; i++) {
        if (send(fd, act->dgram[i], act->dgram_len[i], 0) < 0 && errno != ECONNREFUSED) {
            return -1;
        }
    }
    return 0;
}

enum rdt_reg rdt_register(int fd, const char *hello, int attempts, int interval_ms,
                          char *reply, size_t cap)
{
    uint8_t buf[RDT_RECV_BUF];
    size_t n = 0;

    if (hello == NULL || reply == NULL || cap == 0 || attempts < 1 || interval_ms < 0) {
        return RDT_REG_IO_ERROR;
    }
    reply[0] = '\0';
    for (int attempt = 0; attempt < attempts; attempt++) {
        /* strlen, not strlen + 1: a trailing NUL would reach the relay as part of the last
         * word, and it would answer "ERR role must be recv or send". */
        if (send(fd, hello, strlen(hello), 0) < 0 && errno != ECONNREFUSED) {
            return RDT_REG_IO_ERROR;
        }
        uint64_t deadline = rdt_now_ms() + (uint64_t)interval_ms; /* interval_ms >= 0 */
        for (;;) {
            int got = rdt_wait(fd, rdt_poll_timeout(true, deadline, rdt_now_ms()), buf,
                               sizeof buf, &n);
            /* Excluded from coverage: the socket failing on receive right after it sent
             * successfully, which a test cannot arrange. */
            if (got < 0) { // GCOVR_EXCL_START
                return RDT_REG_IO_ERROR;
            } // GCOVR_EXCL_STOP
            /* The reply carries no terminator, so compare by length, never with strcmp. */
            if (got > 0 && n == 2 && memcmp(buf, "OK", 2) == 0) {
                return RDT_REG_OK;
            }
            if (got > 0 && n >= 3 && memcmp(buf, "ERR", 3) == 0) {
                size_t start = (n > 3 && buf[3] == ' ') ? 4 : 3;
                size_t keep = n - start < cap - 1 ? n - start : cap - 1;
                memcpy(reply, buf + start, keep);
                reply[keep] = '\0';
                return RDT_REG_REFUSED;
            }
            /* Anything else is not for us; keep waiting until this attempt's time is up. */
            if (rdt_now_ms() >= deadline) {
                break;
            }
        }
    }
    return RDT_REG_NO_REPLY;
}

/* ----------------------------------------------------------------------------- files */

int rdt_read_file(const char *path, uint8_t **data, size_t *size)
{
    struct stat st;

    if (path == NULL || data == NULL || size == NULL) {
        errno = EINVAL;
        return -1;
    }
    *data = NULL;
    *size = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    /* Excluded from coverage: fstat failing on a file that was just opened. */
    if (fstat(fileno(f), &st) != 0) { // GCOVR_EXCL_START
        fclose(f);
        return -1;
    } // GCOVR_EXCL_STOP
    if (!S_ISREG(st.st_mode)) {
        fclose(f);
        errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        return -1;
    }
    if (st.st_size > (off_t)RDT_MAX_FILE) {
        fclose(f);
        errno = EFBIG;
        return -1;
    }
    size_t n = (size_t)st.st_size; /* a regular file, 0 to RDT_MAX_FILE bytes */
    uint8_t *buf = malloc(n + 1);  /* + 1 so an empty file still gets a real buffer */
    /* Excluded from coverage: malloc failing, or the file shrinking between fstat and
     * fread. Library-call failures a test cannot force. */
    if (buf == NULL || fread(buf, 1, n, f) != n) { // GCOVR_EXCL_START
        free(buf);
        fclose(f);
        errno = EIO;
        return -1;
    } // GCOVR_EXCL_STOP
    fclose(f);
    *data = buf;
    *size = n;
    return 0;
}

/* ------------------------------------------------------------------------ event loops */

enum rdt_result rdt_sender_step(int fd, struct rdt_sender *s)
{
    uint8_t buf[RDT_RECV_BUF];
    struct rdt_actions act;
    size_t n = 0;

    if (s == NULL || (s->state == RDT_RUNNING && !s->timer_running)) {
        return RDT_IO_ERROR; /* never started, so there is nothing to wait for */
    }
    if (s->state != RDT_RUNNING) {
        return s->state;
    }
    int got = rdt_wait(fd, rdt_poll_timeout(true, s->timer_due_ms, rdt_now_ms()), buf,
                       sizeof buf, &n);
    if (got < 0) {
        return RDT_IO_ERROR;
    }
    if (got > 0) {
        rdt_sender_on_datagram(s, buf, n, rdt_now_ms(), &act);
        if (rdt_send_actions(fd, &act) != 0) {
            return RDT_IO_ERROR;
        }
    }
    /* Checked after a datagram as well, so a steady stream of duplicate ACKs cannot keep the
     * timer from ever being looked at. Does nothing unless it has expired. */
    rdt_sender_on_timeout(s, rdt_now_ms(), &act);
    if (rdt_send_actions(fd, &act) != 0) {
        return RDT_IO_ERROR;
    }
    return act.result;
}

enum rdt_result rdt_sender_run(int fd, struct rdt_sender *s)
{
    struct rdt_actions act;

    if (s == NULL) {
        return RDT_IO_ERROR;
    }
    rdt_sender_start(s, rdt_now_ms(), &act);
    if (rdt_send_actions(fd, &act) != 0) {
        return RDT_IO_ERROR;
    }
    enum rdt_result result = act.result;
    while (result == RDT_RUNNING) {
        result = rdt_sender_step(fd, s);
    }
    return result;
}

/* Carries out a receiver's actions: write, then close at the FIN, then ACK. The write comes
 * first because an ACK promises the bytes are safely in the file. */
static int receiver_carry_out(int fd, const struct rdt_actions *act, FILE **out)
{
    if (act->deliver_len > 0
        && (*out == NULL || fwrite(act->deliver, 1, act->deliver_len, *out) != act->deliver_len)) {
        return -1;
    }
    if (act->fin && *out != NULL) {
        int rc = fclose(*out);
        *out = NULL;
        if (rc != 0) {
            return -1;
        }
    }
    return rdt_send_actions(fd, act);
}

enum rdt_result rdt_receiver_step(int fd, struct rdt_receiver *r, FILE **out)
{
    uint8_t buf[RDT_RECV_BUF];
    struct rdt_actions act;
    size_t n = 0;

    if (r == NULL || out == NULL) {
        return RDT_IO_ERROR;
    }
    if (r->state != RDT_RUNNING) {
        return r->state;
    }
    int got = rdt_wait(fd, rdt_poll_timeout(true, r->deadline_ms, rdt_now_ms()), buf,
                       sizeof buf, &n);
    if (got < 0) {
        return RDT_IO_ERROR;
    }
    if (got > 0) {
        rdt_receiver_on_datagram(r, buf, n, rdt_now_ms(), &act);
        if (receiver_carry_out(fd, &act, out) != 0) {
            return RDT_IO_ERROR;
        }
    }
    rdt_receiver_on_timeout(r, rdt_now_ms(), &act);
    return act.result;
}

enum rdt_result rdt_receiver_run(int fd, struct rdt_receiver *r, FILE **out)
{
    enum rdt_result result;

    do {
        result = rdt_receiver_step(fd, r, out);
    } while (result == RDT_RUNNING);
    return result;
}
