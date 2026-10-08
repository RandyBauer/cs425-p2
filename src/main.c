#include "lab.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

/*
 * Wiring only: parse the command line, open the socket, register, run the transfer, and turn
 * the outcome into an exit code. Every function called here is unit tested; this file is the
 * part gcovr excludes, so it holds no protocol logic.
 *
 * Exit codes: 0 success (and no arguments, which prints usage), 1 when the command line is
 * wrong, 2 when the relay refuses, the network fails, or the transfer gives up.
 *
 * Manual test cases for this file, which unit tests cannot reach (run from the repo root,
 * relay started with: python3 cs425_relay.py --delay 50):
 *
 *   ./build/release/myapp                                   usage on stdout, exit 0
 *   ./build/release/myapp send                              error + usage, exit 1
 *   ./build/release/myapp fly -s a 127.0.0.1 x              unknown mode, exit 1
 *   ./build/release/myapp send -s a -w 0 127.0.0.1 in.bin   bad window, exit 1
 *   ./build/release/myapp send -s a -l 1e-05 127.0.0.1 x    bad rate, exit 1
 *   ./build/release/myapp recv -s a -w 4 127.0.0.1 out.bin  -w is send-only, exit 1
 *   ./build/release/myapp send -s a 127.0.0.1 missing.bin   cannot read file, exit 1
 *   ./build/release/myapp send -s nobody 127.0.0.1 in.bin   relay says ERR no receiver, exit 2
 *   ./build/release/myapp recv -s a -p 4999 127.0.0.1 o.bin no relay: 5 hellos, then exit 2
 *   recv, then send -l 0.1 -c 0.05 -d 0.05 in.bin           cmp identical, both exit 0
 *   recv, then kill the sender                              receiver exits 2 after 30 s
 *   send to a receiver killed mid-transfer                  sender exits 2 after 10 timeouts
 */

/* Builds the hello and registers. Returns 0 on OK, or the exit code to use. */
static int register_with_relay(int fd, const struct rdt_config *cfg)
{
    char hello[128];
    char reply[256];

    if (rdt_hello_text(cfg, hello, sizeof hello) < 0) {
        fprintf(stderr, "myapp: cannot build the hello\n");
        return 2;
    }
    switch (rdt_register(fd, hello, RDT_HELLO_ATTEMPTS, RDT_HELLO_INTERVAL_MS, reply,
                         sizeof reply)) {
    case RDT_REG_OK:
        return 0;
    case RDT_REG_REFUSED:
        fprintf(stderr, "myapp: the relay refused: %s\n", reply);
        return 2;
    case RDT_REG_NO_REPLY:
        fprintf(stderr, "myapp: no reply from the relay at %s port %s after %d hellos; is it "
                        "running, and does -p match its --port?\n",
                cfg->relay, cfg->port, RDT_HELLO_ATTEMPTS);
        return 2;
    case RDT_REG_IO_ERROR:
    default:
        fprintf(stderr, "myapp: socket error while registering: %s\n", strerror(errno));
        return 2;
    }
}

/* myapp send: read the file, register, run the Go-Back-N sender. */
static int run_send(const struct rdt_config *cfg)
{
    uint8_t *data = NULL;
    size_t size = 0;
    char err[256];
    struct rdt_sender sender;

    if (rdt_read_file(cfg->file, &data, &size) != 0) {
        fprintf(stderr, "myapp: cannot read %s: %s\n", cfg->file, strerror(errno));
        return 1;
    }
    int fd = rdt_open(cfg->relay, cfg->port, err, sizeof err);
    if (fd < 0) {
        fprintf(stderr, "myapp: %s\n", err);
        free(data);
        return 2;
    }
    int code = register_with_relay(fd, cfg);
    if (code == 0) {
        if (rdt_sender_init(&sender, data, size, cfg->window, cfg->timeout_ms) != 0) {
            fprintf(stderr, "myapp: out of memory\n");
            code = 2;
        } else {
            switch (rdt_sender_run(fd, &sender)) {
            case RDT_DONE:
                code = 0;
                break;
            case RDT_GAVE_UP:
                fprintf(stderr, "myapp: gave up after %u timeouts in a row with no progress\n",
                        RDT_MAX_TIMEOUTS);
                code = 2;
                break;
            case RDT_RUNNING:
            case RDT_IO_ERROR:
            default:
                fprintf(stderr, "myapp: socket error during the transfer: %s\n", strerror(errno));
                code = 2;
                break;
            }
        }
        rdt_sender_destroy(&sender);
    }
    close(fd);
    free(data);
    return code;
}

/* myapp recv: open the output, register, run the Go-Back-N receiver. */
static int run_recv(const struct rdt_config *cfg)
{
    char err[256];
    struct rdt_receiver receiver;

    FILE *out = fopen(cfg->file, "wb");
    if (out == NULL) {
        fprintf(stderr, "myapp: cannot write %s: %s\n", cfg->file, strerror(errno));
        return 1;
    }
    int fd = rdt_open(cfg->relay, cfg->port, err, sizeof err);
    if (fd < 0) {
        fprintf(stderr, "myapp: %s\n", err);
        fclose(out);
        return 2;
    }
    int code = register_with_relay(fd, cfg);
    if (code == 0) {
        rdt_receiver_init(&receiver, rdt_now_ms(), RDT_IDLE_MS, RDT_LINGER_MS);
        switch (rdt_receiver_run(fd, &receiver, &out)) {
        case RDT_DONE:
            code = 0;
            break;
        case RDT_GAVE_UP:
            fprintf(stderr, "myapp: nothing valid arrived for %u seconds; giving up\n",
                    RDT_IDLE_MS / 1000u);
            code = 2;
            break;
        case RDT_RUNNING:
        case RDT_IO_ERROR:
        default:
            fprintf(stderr, "myapp: error during the transfer: %s\n", strerror(errno));
            code = 2;
            break;
        }
    }
    /* Already closed if the FIN arrived; otherwise close what was written so far. */
    if (out != NULL) {
        fclose(out);
    }
    close(fd);
    return code;
}

int main(int argc, char *argv[])
{
    struct rdt_config cfg;
    char err[256];

    if (argc < 2) {
        rdt_usage(stdout);
        return 0;
    }
    if (rdt_parse_args(argc, argv, &cfg, err, sizeof err) != 0) {
        fprintf(stderr, "myapp: %s\n\n", err);
        rdt_usage(stderr);
        return 1;
    }
    return cfg.mode == RDT_MODE_SEND ? run_send(&cfg) : run_recv(&cfg);
}
