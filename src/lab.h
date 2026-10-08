#ifndef LAB_H
#define LAB_H

/*
 * P2 - Reliable Data Transfer: Go-Back-N over UDP, through the CS425 relay.
 *
 * The program is three layers, and this header is split the same way:
 *
 *   1. Packets         Bytes in, bytes or a struct out. No state, no I/O.
 *   2. State machines  The Go-Back-N sender and receiver. The current time is a parameter,
 *                      and every event function fills in the actions to carry out. Nothing
 *                      here calls sendto, recvfrom, poll, clock_gettime or fwrite.
 *   3. I/O             The command line, the socket, the relay hello, the poll loop, the
 *                      clock and the files. The only layer that touches the outside world.
 *
 * Each layer uses only the layers above it in this file. The unit tests drive layer 2 with
 * a made-up clock and an in-memory channel, which is why it must not know what a socket is.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ======================================================================================
 * Layer 1: packets
 * ====================================================================================== */

/** Bytes in every packet header: type, reserved, checksum, seq, length. */
#define RDT_HEADER_LEN 10u
/** Largest payload a DATA packet may carry. */
#define RDT_MAX_PAYLOAD 1024u
/** Largest legal packet on the wire. */
#define RDT_MAX_PACKET (RDT_HEADER_LEN + RDT_MAX_PAYLOAD)
/**
 * Receive buffer size. Deliberately larger than RDT_MAX_PACKET: UDP truncates a datagram
 * that does not fit, so a buffer of exactly the maximum would hide an oversized one.
 */
#define RDT_RECV_BUF 2048u

/** Packet types, the first byte on the wire. */
enum rdt_type {
    RDT_DATA = 0, /**< carries part of the file */
    RDT_ACK = 1,  /**< cumulative: every packet below seq has arrived */
    RDT_FIN = 2   /**< end of file; seq is the number of DATA packets */
};

/** Why rdt_decode rejected a datagram. Every rejection is treated as a lost packet. */
enum rdt_status {
    RDT_OK = 0,      /**< valid packet */
    RDT_ERR_ARG,     /**< NULL buffer or output */
    RDT_ERR_SHORT,   /**< fewer than RDT_HEADER_LEN bytes */
    RDT_ERR_LENGTH,  /**< length field over 1024, or 10 + length != datagram size */
    RDT_ERR_TYPE,    /**< type not 0, 1 or 2, or reserved byte not 0 */
    RDT_ERR_CHECKSUM /**< the Internet checksum does not verify */
};

/** A decoded packet. Multi-byte fields are in host byte order here. */
struct rdt_packet {
    uint8_t type;           /**< one of enum rdt_type */
    uint32_t seq;           /**< packet index (DATA, FIN) or next index expected (ACK) */
    uint16_t len;           /**< payload bytes, 0 to RDT_MAX_PAYLOAD */
    const uint8_t *payload; /**< len bytes, or NULL when len is 0; not owned */
};

/**
 * @brief Computes the RFC 1071 Internet checksum of a byte range.
 *
 * The bytes are summed as big-endian 16-bit words with end-around carry, an odd final byte
 * counting as the high half of a word whose low half is zero, and the one's complement of
 * the sum is returned. The result is in host byte order and matches the handout's numbers:
 * 00 01 f2 03 f4 f5 f6 f7 gives 0x220d. Store it with htons.
 *
 * Run over a packet whose checksum field is filled in, it returns 0 exactly when the packet
 * is intact (the sum is 0xffff).
 *
 * @param data The bytes to sum. NULL is treated as an empty range.
 * @param len  Number of bytes.
 * @return The checksum, in host byte order.
 */
uint16_t rdt_checksum(const uint8_t *data, size_t len);

/**
 * @brief Encodes a packet into a buffer in the wire format, checksum included.
 *
 * Fields are written one at a time in network byte order, never by copying a struct.
 *
 * @param p   The packet. Its type must be valid, len at most RDT_MAX_PAYLOAD, len 0 for
 *            ACK and FIN, and payload non-NULL whenever len is not 0.
 * @param buf Where to write the packet.
 * @param cap Size of buf in bytes.
 * @return Bytes written (RDT_HEADER_LEN + len), or 0 if the packet is invalid or buf is
 *         NULL or too small.
 */
size_t rdt_encode(const struct rdt_packet *p, uint8_t *buf, size_t cap);

/**
 * @brief Validates a received datagram and decodes it into a packet.
 *
 * Checks run in the handout's order, and all of them finish before anything is read from
 * the payload: at least 10 bytes; 10 + length equals len and length is at most 1024; type
 * is 0, 1 or 2 and reserved is 0; the checksum verifies.
 *
 * @param buf The datagram as received.
 * @param len The size the receive call reported, which is trusted over the length field.
 * @param out Filled in on success. Its payload points into buf, so buf must outlive it.
 * @return RDT_OK, or the first check that failed.
 */
enum rdt_status rdt_decode(const uint8_t *buf, size_t len, struct rdt_packet *out);

/* ======================================================================================
 * Layer 2: the Go-Back-N state machines
 * ====================================================================================== */

/** Largest window the command line accepts, and so the most datagrams one event can send. */
#define RDT_MAX_WINDOW 64u
/** Consecutive timeouts without progress after which the sender gives up. */
#define RDT_MAX_TIMEOUTS 10u
/** Largest file the project handles: 16 MiB, so a 32-bit seq never wraps. */
#define RDT_MAX_FILE (16u * 1024u * 1024u)

/** Where a state machine stands, and what each event function reports. */
enum rdt_result {
    RDT_RUNNING = 0, /**< keep going */
    RDT_DONE,        /**< transfer complete (sender: FIN acknowledged; receiver: linger over) */
    RDT_GAVE_UP,     /**< sender: 10 fruitless timeouts; receiver: 30 s with nothing valid */
    RDT_IO_ERROR     /**< reported only by layer 3: the socket or a file failed */
};

/**
 * What the caller must do after an event. Layer 2 decides; layer 3 carries it out.
 * Pointers stay valid until the next call into the same state machine.
 */
struct rdt_actions {
    size_t ndgram;                           /**< datagrams to send, in order */
    const uint8_t *dgram[RDT_MAX_WINDOW];    /**< datagram bytes, owned by the state machine */
    size_t dgram_len[RDT_MAX_WINDOW];        /**< datagram sizes */
    const uint8_t *deliver;                  /**< receiver: payload to append to the file */
    size_t deliver_len;                      /**< receiver: payload bytes, 0 for none */
    bool fin;                                /**< receiver: the FIN arrived, close the file */
    bool timer_set;                          /**< is a deadline pending? */
    uint64_t timer_due_ms;                   /**< when to call the timeout function */
    enum rdt_result result;                  /**< the state machine's state after the event */
};

/** One retransmission copy: an encoded packet kept until it is acknowledged. */
struct rdt_slot {
    uint8_t bytes[RDT_MAX_PACKET]; /**< the packet exactly as first sent */
    size_t len;                    /**< its size */
};

/**
 * The Go-Back-N sender. Packets 0 to total - 1 are DATA; packet total is the FIN.
 * Everything below base is acknowledged; base to next - 1 is in flight, each with a copy
 * in slots[seq % window]; next is the next packet never sent.
 */
struct rdt_sender {
    const uint8_t *data;   /**< the whole file, not owned */
    size_t size;           /**< file size in bytes */
    uint32_t window;       /**< N: at most this many packets in flight */
    uint64_t timeout_ms;   /**< retransmission timeout */
    uint32_t total;        /**< number of DATA packets, which is also the FIN's seq */
    uint32_t base;         /**< oldest packet not yet acknowledged */
    uint32_t next;         /**< next packet never sent */
    bool timer_running;    /**< the single timer, running while anything is in flight */
    uint64_t timer_due_ms; /**< when it expires */
    unsigned fruitless;    /**< timeouts in a row with no ACK that moved base */
    enum rdt_result state; /**< RDT_RUNNING until done or given up */
    struct rdt_slot *slots;/**< window retransmission copies, malloc'd by init */
};

/**
 * @brief Prepares a sender for a file. Call rdt_sender_destroy when finished, even if this
 * fails.
 *
 * @param s          The sender to initialize.
 * @param data       The file's bytes. May be NULL only when size is 0. Not copied, so it
 *                   must outlive the sender.
 * @param size       File size, at most RDT_MAX_FILE.
 * @param window     Window size, 1 to RDT_MAX_WINDOW.
 * @param timeout_ms Retransmission timeout, greater than 0.
 * @return 0 on success, -1 on a bad argument or if memory runs out.
 */
int rdt_sender_init(struct rdt_sender *s, const uint8_t *data, size_t size, uint32_t window,
                    uint64_t timeout_ms);

/**
 * @brief Frees the sender's retransmission copies. Safe to call twice, or after a failed
 * init.
 *
 * @param s The sender, or NULL.
 */
void rdt_sender_destroy(struct rdt_sender *s);

/**
 * @brief Starts the transfer: sends as much of the first window as the file fills, or the
 * FIN alone for an empty file, and starts the timer.
 *
 * @param s      An initialized sender.
 * @param now_ms The current time in milliseconds.
 * @param out    Filled with the datagrams to send and the timer.
 */
void rdt_sender_start(struct rdt_sender *s, uint64_t now_ms, struct rdt_actions *out);

/**
 * @brief Feeds the sender a datagram that arrived from the relay.
 *
 * Damaged or malformed datagrams, and anything but an ACK, are ignored as if lost. An ACK
 * with seq <= base is a duplicate and is ignored: no state changes, the timer included. An
 * ACK with base < seq <= next slides base to seq (cumulative, so possibly by several
 * packets), resets the give-up count, sends whatever the window now allows (the FIN once
 * every DATA packet is acknowledged) and restarts the timer. An ACK beyond next cannot come
 * from a correct receiver and is ignored. The ACK for the FIN finishes the transfer.
 *
 * @param s      The sender.
 * @param buf    The datagram.
 * @param len    Its size as received.
 * @param now_ms The current time in milliseconds.
 * @param out    Filled with the datagrams to send, the timer and the result.
 */
void rdt_sender_on_datagram(struct rdt_sender *s, const uint8_t *buf, size_t len,
                            uint64_t now_ms, struct rdt_actions *out);

/**
 * @brief Tells the sender time has passed. Does nothing unless the timer has expired.
 *
 * On expiry this is the "go back": every packet from base to next - 1 is resent from its
 * copy and the timer restarts. The 10th expiry in a row without progress gives up instead,
 * without resending.
 *
 * @param s      The sender.
 * @param now_ms The current time in milliseconds.
 * @param out    Filled with the datagrams to send, the timer and the result.
 */
void rdt_sender_on_timeout(struct rdt_sender *s, uint64_t now_ms, struct rdt_actions *out);

/** The Go-Back-N receiver: one number, expected, and in-order delivery. */
struct rdt_receiver {
    uint32_t expected;         /**< index of the next packet wanted */
    bool finished;             /**< the FIN has arrived and the receiver is lingering */
    uint64_t deadline_ms;      /**< idle limit, or the end of the linger once finished */
    uint64_t idle_ms;          /**< give up after this long with nothing valid */
    uint64_t linger_ms;        /**< how long to answer repeated FINs */
    enum rdt_result state;     /**< RDT_RUNNING until the linger ends or it gives up */
    uint8_t ack[RDT_HEADER_LEN]; /**< the encoded ACK handed out in rdt_actions */
};

/**
 * @brief Prepares a receiver, expecting packet 0.
 *
 * @param r         The receiver.
 * @param now_ms    The current time in milliseconds; the idle limit counts from here.
 * @param idle_ms   Give up after this long with nothing valid (the program uses 30000).
 * @param linger_ms Linger after the FIN this long (the program uses 2000).
 */
void rdt_receiver_init(struct rdt_receiver *r, uint64_t now_ms, uint64_t idle_ms,
                       uint64_t linger_ms);

/**
 * @brief Feeds the receiver a datagram that arrived from the relay.
 *
 * A damaged or malformed datagram gets no reply at all. A DATA packet with seq == expected
 * is delivered and acknowledged with the new expected. Any other DATA or FIN (a duplicate,
 * or one from beyond a gap) is discarded and expected is ACKed again. A FIN with seq ==
 * expected asks for the file to be closed, is ACKed, and starts the linger, during which
 * repeated FINs get the same ACK. Every valid packet resets the idle limit until the FIN.
 *
 * @param r      The receiver.
 * @param buf    The datagram. A delivered payload points into it.
 * @param len    Its size as received.
 * @param now_ms The current time in milliseconds.
 * @param out    Filled with the ACK to send, any payload to deliver, and the deadline.
 */
void rdt_receiver_on_datagram(struct rdt_receiver *r, const uint8_t *buf, size_t len,
                              uint64_t now_ms, struct rdt_actions *out);

/**
 * @brief Tells the receiver time has passed. Once the deadline passes it is done if the
 * FIN arrived (the linger is over), and gives up if not (nothing valid for idle_ms).
 *
 * @param r      The receiver.
 * @param now_ms The current time in milliseconds.
 * @param out    Filled with the deadline and the result. Never asks to send anything.
 */
void rdt_receiver_on_timeout(struct rdt_receiver *r, uint64_t now_ms, struct rdt_actions *out);

/* ======================================================================================
 * Layer 3: I/O - command line, socket, relay hello, poll loop, clock, files
 * ====================================================================================== */

#define RDT_DEFAULT_WINDOW 8u        /**< -w default */
#define RDT_DEFAULT_TIMEOUT_MS 250u  /**< -T default */
#define RDT_DEFAULT_PORT "4250"      /**< -p default */
#define RDT_MAX_TIMEOUT_MS 60000u    /**< largest -T accepted */
#define RDT_HELLO_ATTEMPTS 5         /**< hellos sent before giving up on the relay */
#define RDT_HELLO_INTERVAL_MS 1000   /**< wait for a reply this long per hello */
#define RDT_IDLE_MS 30000u           /**< receiver gives up after this long with nothing */
#define RDT_LINGER_MS 2000u          /**< receiver lingers this long after the FIN */

/** Which side of the transfer this run is. */
enum rdt_mode {
    RDT_MODE_SEND, /**< myapp send */
    RDT_MODE_RECV  /**< myapp recv */
};

/** The parsed command line. Strings point into argv. */
struct rdt_config {
    enum rdt_mode mode;  /**< from argv[1] */
    const char *session; /**< -s, 1 to 32 of a-z, 0-9 and - */
    uint32_t window;     /**< -w, 1 to 64 */
    uint32_t timeout_ms; /**< -T, 1 to RDT_MAX_TIMEOUT_MS */
    const char *loss;    /**< -l, as typed: decimal digits, 0 to 0.5 */
    const char *corrupt; /**< -c, as typed */
    const char *dup;     /**< -d, as typed */
    const char *port;    /**< -p, 1 to 65535, as typed */
    const char *relay;   /**< host name or address of the relay */
    const char *file;    /**< file to send, or file to write */
};

/** How registering with the relay went. */
enum rdt_reg {
    RDT_REG_OK,       /**< the relay answered OK */
    RDT_REG_REFUSED,  /**< the relay answered ERR; the reason is in the reply buffer */
    RDT_REG_NO_REPLY, /**< no answer after every attempt */
    RDT_REG_IO_ERROR  /**< bad argument or the socket failed */
};

/**
 * @brief Parses the command line: the mode from argv[1], the rest with getopt.
 *
 * Rates are validated with the relay's own rule (decimal digits with at most one point, no
 * sign or exponent, 0 to 0.5) and kept as typed, so a hello the relay would reject is caught
 * here as a command-line error instead. Safe to call more than once per process.
 *
 * @param argc   Argument count, at least 2.
 * @param argv   Argument vector. getopt may reorder its pointers.
 * @param cfg    Filled in on success.
 * @param err    Receives a one-line reason on failure.
 * @param errcap Size of err.
 * @return 0 on success, -1 if the command line is wrong.
 */
int rdt_parse_args(int argc, char *argv[], struct rdt_config *cfg, char *err, size_t errcap);

/**
 * @brief Prints the usage message.
 *
 * @param f Where to print it, or NULL to print nothing.
 */
void rdt_usage(FILE *f);

/**
 * @brief Builds the registration hello: "HELLO <session> recv" or
 * "HELLO <session> send <loss> <corrupt> <dup>", with no newline.
 *
 * @param cfg The parsed command line.
 * @param buf Where to write the text, NUL-terminated.
 * @param cap Size of buf.
 * @return The text's length (which is what to send: no NUL), or -1 if it does not fit or an
 *         argument is NULL.
 */
int rdt_hello_text(const struct rdt_config *cfg, char *buf, size_t cap);

/**
 * @brief Reads CLOCK_MONOTONIC, which never jumps, unlike the wall clock.
 *
 * @return Milliseconds since an arbitrary fixed point.
 */
uint64_t rdt_now_ms(void);

/**
 * @brief Converts a deadline into a timeout for poll.
 *
 * @param timer_set Whether a deadline exists.
 * @param due_ms    The deadline.
 * @param now_ms    The current time.
 * @return -1 (wait forever) if no deadline, 0 if it has passed (never negative, which poll
 *         would read as forever), otherwise the milliseconds left, capped at INT_MAX.
 */
int rdt_poll_timeout(bool timer_set, uint64_t due_ms, uint64_t now_ms);

/**
 * @brief Resolves the relay with getaddrinfo and opens the one UDP socket used for the whole
 * run, connected to the relay so that datagrams from anywhere else are filtered out.
 *
 * IPv4 only, because the relay is: asking for any family would let "localhost" resolve to
 * ::1, where nothing is listening.
 *
 * @param host   Relay host name or address.
 * @param port   Relay port, as text.
 * @param err    Receives a one-line reason on failure.
 * @param errcap Size of err.
 * @return The socket, or -1.
 */
int rdt_open(const char *host, const char *port, char *err, size_t errcap);

/**
 * @brief Waits up to timeout_ms for one datagram.
 *
 * A connected UDP socket reports an ICMP "port unreachable" as ECONNREFUSED; that is
 * treated as nothing arriving, so a relay that is not there looks like silence.
 *
 * @param fd         The socket.
 * @param timeout_ms As for poll: -1 waits forever, 0 does not wait.
 * @param buf        Receive buffer; give it RDT_RECV_BUF bytes.
 * @param cap        Size of buf.
 * @param len        Receives the datagram's size.
 * @return 1 if a datagram arrived, 0 if not, -1 on a socket error or NULL argument.
 */
int rdt_wait(int fd, int timeout_ms, uint8_t *buf, size_t cap, size_t *len);

/**
 * @brief Sends every datagram in an actions list, in order. ECONNREFUSED (left over from an
 * earlier datagram) counts as sent, the same as a datagram lost on the way.
 *
 * @param fd  The socket.
 * @param act The actions.
 * @return 0 on success, -1 on a socket error or NULL argument.
 */
int rdt_send_actions(int fd, const struct rdt_actions *act);

/**
 * @brief Registers with the relay: sends the hello, waits interval_ms for "OK" or
 * "ERR <reason>", and sends it again if nothing comes, up to attempts times in all.
 * Other datagrams arriving meanwhile are ignored.
 *
 * @param fd          The socket.
 * @param hello       The hello text, sent without its NUL.
 * @param attempts    Hellos to send at most, at least 1.
 * @param interval_ms How long to wait after each, at least 0.
 * @param reply       Receives the ERR reason, NUL-terminated, or "".
 * @param cap         Size of reply, at least 1.
 * @return How it went.
 */
enum rdt_reg rdt_register(int fd, const char *hello, int attempts, int interval_ms,
                          char *reply, size_t cap);

/**
 * @brief Reads a whole regular file of at most RDT_MAX_FILE bytes into memory.
 *
 * @param path The file.
 * @param data Receives a malloc'd buffer the caller frees (non-NULL even for an empty file).
 * @param size Receives the size.
 * @return 0 on success, -1 with errno set (EISDIR, EINVAL for other non-regular files,
 *         EFBIG if too large, or whatever fopen or fstat reported).
 */
int rdt_read_file(const char *path, uint8_t **data, size_t *size);

/**
 * @brief One turn of the sender's loop: waits for an ACK or the timer, feeds the state
 * machine, sends what it asks for.
 *
 * @param fd The socket.
 * @param s  The sender.
 * @return The sender's state, or RDT_IO_ERROR.
 */
enum rdt_result rdt_sender_step(int fd, struct rdt_sender *s);

/**
 * @brief Runs a whole transfer: starts the sender, then steps until it finishes.
 *
 * @param fd The socket, already registered.
 * @param s  An initialized sender.
 * @return RDT_DONE, RDT_GAVE_UP or RDT_IO_ERROR.
 */
enum rdt_result rdt_sender_run(int fd, struct rdt_sender *s);

/**
 * @brief One turn of the receiver's loop: waits for a datagram or the deadline, feeds the
 * state machine, writes what it delivers, closes the file at the FIN, sends the ACK.
 *
 * @param fd  The socket.
 * @param r   The receiver.
 * @param out The output file. Closed and set to NULL when the FIN arrives.
 * @return The receiver's state, or RDT_IO_ERROR (including a failed write or close).
 */
enum rdt_result rdt_receiver_step(int fd, struct rdt_receiver *r, FILE **out);

/**
 * @brief Runs the receiving side until the linger ends or it gives up.
 *
 * @param fd  The socket, already registered.
 * @param r   An initialized receiver.
 * @param out The output file. Closed and set to NULL when the FIN arrives.
 * @return RDT_DONE, RDT_GAVE_UP or RDT_IO_ERROR.
 */
enum rdt_result rdt_receiver_run(int fd, struct rdt_receiver *r, FILE **out);

#endif /* LAB_H */
