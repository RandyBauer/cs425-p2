#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "harness/unity.h"
#include "../src/lab.h"

/*
 * Tests for all three layers. Layers 1 and 2 are driven with plain buffers, a made-up clock
 * and an in-memory channel. Layer 3 is driven over real UDP sockets on 127.0.0.1 with no
 * relay: a datagram sent before anyone reads waits in the kernel's buffer, so a test can
 * queue the "relay's" reply first and then call the function that waits for it.
 */

void setUp(void)
{
}

void tearDown(void)
{
}

/* ============================================================================ helpers */

/* Encodes a packet with a text payload (or none) and returns its size. */
static size_t make_packet(uint8_t *buf, uint8_t type, uint32_t seq, const char *text)
{
    struct rdt_packet p = { type, seq, 0, NULL };
    if (text != NULL) {
        p.len = (uint16_t)strlen(text);
        p.payload = (const uint8_t *)text;
    }
    size_t n = rdt_encode(&p, buf, RDT_MAX_PACKET);
    TEST_ASSERT_TRUE(n >= RDT_HEADER_LEN);
    return n;
}

/* Decodes datagram i of an actions list, failing the test if it is not a valid packet. */
static struct rdt_packet action_packet(const struct rdt_actions *a, size_t i)
{
    struct rdt_packet p;
    TEST_ASSERT_TRUE(i < a->ndgram);
    TEST_ASSERT_EQUAL_INT(RDT_OK, rdt_decode(a->dgram[i], a->dgram_len[i], &p));
    return p;
}

/* Feeds the receiver datagram i of a sender's actions, and returns the ACK it sends back, or
 * -1 if it sends nothing. */
static long feed_receiver(struct rdt_receiver *r, const struct rdt_actions *from, size_t i,
                          uint64_t now, struct rdt_actions *act)
{
    rdt_receiver_on_datagram(r, from->dgram[i], from->dgram_len[i], now, act);
    if (act->ndgram == 0) {
        return -1;
    }
    struct rdt_packet ack = action_packet(act, 0);
    TEST_ASSERT_EQUAL_UINT8(RDT_ACK, ack.type);
    return (long)ack.seq;
}

/* Feeds the sender "ACK seq". */
static void ack_sender(struct rdt_sender *s, uint32_t seq, uint64_t now, struct rdt_actions *act)
{
    uint8_t buf[RDT_MAX_PACKET];
    size_t n = make_packet(buf, RDT_ACK, seq, NULL);
    rdt_sender_on_datagram(s, buf, n, now, act);
}

/* Asserts an actions list holds exactly the packets first..first+count-1, in order. */
static void assert_sends(const struct rdt_actions *a, uint32_t first, size_t count)
{
    TEST_ASSERT_EQUAL_UINT((unsigned)count, (unsigned)a->ndgram);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32(first + (uint32_t)i, action_packet(a, i).seq);
    }
}

/* A small seeded generator (xorshift32), so every run and every machine sees the same
 * damage. Kept in a struct rather than a global. */
struct rng {
    uint32_t state;
};

static uint32_t rng_next(struct rng *g)
{
    uint32_t x = g->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->state = x;
    return x;
}

static double rng_unit(struct rng *g)
{
    return (double)rng_next(g) / 4294967296.0;
}

/* Bytes of a test file, from a seed. The caller frees. */
static uint8_t *make_file(size_t size, uint32_t seed)
{
    struct rng g = { seed };
    uint8_t *data = malloc(size + 1);
    TEST_ASSERT_NOT_NULL(data);
    for (size_t i = 0; i < size; i++) {
        data[i] = (uint8_t)rng_next(&g);
    }
    return data;
}

/* An unconnected UDP socket on 127.0.0.1 with a kernel-chosen port. */
static int udp_bound(struct sockaddr_in *addr)
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, bind(fd, (struct sockaddr *)&a, sizeof a));
    TEST_ASSERT_EQUAL_INT(0, getsockname(fd, (struct sockaddr *)&a, &len));
    if (addr != NULL) {
        *addr = a;
    }
    return fd;
}

/* Two UDP sockets on 127.0.0.1, connected to each other. */
static void udp_pair(int *a, int *b)
{
    struct sockaddr_in addr_a;
    struct sockaddr_in addr_b;
    *a = udp_bound(&addr_a);
    *b = udp_bound(&addr_b);
    TEST_ASSERT_EQUAL_INT(0, connect(*a, (struct sockaddr *)&addr_b, sizeof addr_b));
    TEST_ASSERT_EQUAL_INT(0, connect(*b, (struct sockaddr *)&addr_a, sizeof addr_a));
}

/* A port on 127.0.0.1 with nothing listening: take one, note it, let it go. */
static void dead_port(char *text, size_t cap)
{
    struct sockaddr_in a;
    close(udp_bound(&a));
    snprintf(text, cap, "%u", (unsigned)ntohs(a.sin_port));
}

/* Receives one datagram without waiting. Loopback delivery happens inside the sender's
 * system call, so anything sent is already here. Returns its size, or 0 if none. */
static size_t recv_now(int fd, uint8_t *buf)
{
    ssize_t n = recv(fd, buf, RDT_RECV_BUF, MSG_DONTWAIT);
    return n > 0 ? (size_t)n : 0;
}

/* Sends a packet from fd. */
static void send_packet(int fd, uint8_t type, uint32_t seq, const char *text)
{
    uint8_t buf[RDT_MAX_PACKET];
    size_t n = make_packet(buf, type, seq, text);
    TEST_ASSERT_EQUAL_INT((int)n, (int)send(fd, buf, n, 0));
}

/* Creates an empty temporary file and returns its path in buf. */
static void temp_path(char *buf, size_t cap)
{
    snprintf(buf, cap, "/tmp/p2-test-XXXXXX");
    int fd = mkstemp(buf);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
}

/* ============================================================== layer 1: the checksum */

void test_checksum_rfc1071_example(void)
{
    /* RFC 1071 section 3: these bytes sum to 0xddf2, so the checksum is 0x220d. */
    const uint8_t bytes[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    TEST_ASSERT_EQUAL_HEX16(0x220d, rdt_checksum(bytes, sizeof bytes));
}

void test_checksum_odd_length(void)
{
    /* The handout's "Hi!" packet with its checksum field zeroed: 13 bytes. The odd last byte
     * counts as 0x2100, not 0x0021, giving 0x9691. */
    const uint8_t hi[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03,
                           0x48, 0x69, 0x21 };
    TEST_ASSERT_EQUAL_HEX16(0x9691, rdt_checksum(hi, sizeof hi));
    const uint8_t one[] = { 0xab };
    TEST_ASSERT_EQUAL_HEX16(0x54ff, rdt_checksum(one, 1)); /* ~0xab00 */
}

void test_checksum_of_nothing(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xffff, rdt_checksum(NULL, 10)); /* NULL counts as empty */
    const uint8_t any[] = { 0x12 };
    TEST_ASSERT_EQUAL_HEX16(0xffff, rdt_checksum(any, 0));
}

void test_checksum_folds_carries(void)
{
    /* 0xffff + 0xffff + 0x0002 = 0x20000; folding twice gives 0x0002, so ~ is 0xfffd. */
    const uint8_t bytes[] = { 0xff, 0xff, 0xff, 0xff, 0x00, 0x02 };
    TEST_ASSERT_EQUAL_HEX16(0xfffd, rdt_checksum(bytes, sizeof bytes));
}

void test_checksum_intact_packet_verifies_to_zero(void)
{
    /* With the checksum left in, an undamaged packet sums to 0xffff, complemented to 0. */
    const uint8_t hi[] = { 0x00, 0x00, 0x96, 0x91, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03,
                           0x48, 0x69, 0x21 };
    TEST_ASSERT_EQUAL_HEX16(0x0000, rdt_checksum(hi, sizeof hi));
}

void test_checksum_catches_every_single_bit_flip(void)
{
    uint8_t buf[RDT_MAX_PACKET];
    size_t n = make_packet(buf, RDT_DATA, 2, "Hi!");
    for (size_t bit = 0; bit < n * 8; bit++) {
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        TEST_ASSERT_NOT_EQUAL(0, rdt_checksum(buf, n));
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    TEST_ASSERT_EQUAL_HEX16(0x0000, rdt_checksum(buf, n)); /* restored */
}

/* ======================================================= layer 1: encode and decode */

void test_encode_hi_packet_matches_handout(void)
{
    const uint8_t expected[] = { 0x00, 0x00, 0x96, 0x91, 0x00, 0x00, 0x00, 0x02, 0x00, 0x03,
                                 0x48, 0x69, 0x21 };
    uint8_t buf[RDT_MAX_PACKET];
    TEST_ASSERT_EQUAL_UINT(13, (unsigned)make_packet(buf, RDT_DATA, 2, "Hi!"));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, sizeof expected);
}

void test_encode_ack3_matches_handout(void)
{
    const uint8_t expected[] = { 0x01, 0x00, 0xfe, 0xfc, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00 };
    uint8_t buf[RDT_MAX_PACKET];
    TEST_ASSERT_EQUAL_UINT(10, (unsigned)make_packet(buf, RDT_ACK, 3, NULL));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, sizeof expected);
}

void test_encode_full_payload(void)
{
    uint8_t *data = make_file(RDT_MAX_PAYLOAD, 3);
    uint8_t buf[RDT_MAX_PACKET];
    struct rdt_packet p = { RDT_DATA, 0x01020304, RDT_MAX_PAYLOAD, data };
    TEST_ASSERT_EQUAL_UINT(RDT_MAX_PACKET, (unsigned)rdt_encode(&p, buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[4]); /* seq in network order */
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[7]);
    TEST_ASSERT_EQUAL_HEX8(0x04, buf[8]); /* length 1024 = 0x0400 */
    TEST_ASSERT_EQUAL_HEX8(0x00, buf[9]);
    TEST_ASSERT_EQUAL_MEMORY(data, buf + RDT_HEADER_LEN, RDT_MAX_PAYLOAD);
    free(data);
}

void test_encode_rejects_bad_input(void)
{
    uint8_t buf[RDT_MAX_PACKET];
    const uint8_t x[] = { 'x' };
    struct rdt_packet ok = { RDT_DATA, 0, 1, x };
    TEST_ASSERT_EQUAL_UINT(11, (unsigned)rdt_encode(&ok, buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)rdt_encode(NULL, buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)rdt_encode(&ok, NULL, sizeof buf));
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)rdt_encode(&ok, buf, 10)); /* too small */

    struct rdt_packet bad_type = { 3, 0, 0, NULL };
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)rdt_encode(&bad_type, buf, sizeof buf));
    struct rdt_packet too_long = { RDT_DATA, 0, RDT_MAX_PAYLOAD + 1, x };
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)rdt_encode(&too_long, buf, sizeof buf));
    struct rdt_packet ack_payload = { RDT_ACK, 0, 1, x };
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)rdt_encode(&ack_payload, buf, sizeof buf));
    struct rdt_packet no_payload = { RDT_DATA, 0, 1, NULL };
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)rdt_encode(&no_payload, buf, sizeof buf));
}

void test_decode_round_trip(void)
{
    uint8_t *data = make_file(700, 5);
    uint8_t buf[RDT_MAX_PACKET];
    struct rdt_packet in = { RDT_DATA, 77, 700, data };
    struct rdt_packet out;
    size_t n = rdt_encode(&in, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(RDT_OK, rdt_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(RDT_DATA, out.type);
    TEST_ASSERT_EQUAL_UINT32(77, out.seq);
    TEST_ASSERT_EQUAL_UINT16(700, out.len);
    TEST_ASSERT_EQUAL_PTR(buf + RDT_HEADER_LEN, out.payload); /* points into the datagram */
    TEST_ASSERT_EQUAL_MEMORY(data, out.payload, 700);
    free(data);
}

void test_decode_ack_and_fin_have_no_payload(void)
{
    uint8_t buf[RDT_MAX_PACKET];
    struct rdt_packet out;
    size_t n = make_packet(buf, RDT_FIN, 3, NULL);
    TEST_ASSERT_EQUAL_INT(RDT_OK, rdt_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(RDT_FIN, out.type);
    TEST_ASSERT_EQUAL_UINT16(0, out.len);
    TEST_ASSERT_NULL(out.payload);
}

void test_decode_too_short(void)
{
    uint8_t buf[RDT_MAX_PACKET];
    struct rdt_packet out;
    make_packet(buf, RDT_ACK, 1, NULL);
    for (size_t len = 0; len < RDT_HEADER_LEN; len++) {
        TEST_ASSERT_EQUAL_INT(RDT_ERR_SHORT, rdt_decode(buf, len, &out));
    }
}

void test_decode_length_does_not_match_size(void)
{
    uint8_t buf[RDT_RECV_BUF];
    struct rdt_packet out;
    size_t n = make_packet(buf, RDT_DATA, 0, "abcd");
    buf[n] = 0;
    TEST_ASSERT_EQUAL_INT(RDT_ERR_LENGTH, rdt_decode(buf, n + 1, &out)); /* a byte extra */
    TEST_ASSERT_EQUAL_INT(RDT_ERR_LENGTH, rdt_decode(buf, n - 1, &out)); /* a byte short */
}

void test_decode_length_over_1024_even_if_size_matches(void)
{
    /* A 1035-byte datagram claiming 1025 bytes of payload: consistent, but too big. */
    uint8_t buf[RDT_RECV_BUF];
    struct rdt_packet out;
    memset(buf, 0, sizeof buf);
    buf[8] = 0x04;
    buf[9] = 0x01;
    TEST_ASSERT_EQUAL_INT(RDT_ERR_LENGTH, rdt_decode(buf, RDT_HEADER_LEN + 1025, &out));
}

void test_decode_unknown_type_or_reserved(void)
{
    uint8_t buf[RDT_MAX_PACKET];
    struct rdt_packet out;
    size_t n = make_packet(buf, RDT_ACK, 1, NULL);
    buf[0] = 3; /* unknown type; the type check runs before the checksum */
    TEST_ASSERT_EQUAL_INT(RDT_ERR_TYPE, rdt_decode(buf, n, &out));
    buf[0] = RDT_ACK;
    buf[1] = 1; /* reserved must be 0 */
    TEST_ASSERT_EQUAL_INT(RDT_ERR_TYPE, rdt_decode(buf, n, &out));
}

void test_decode_bad_checksum(void)
{
    uint8_t buf[RDT_MAX_PACKET];
    struct rdt_packet out;
    size_t n = make_packet(buf, RDT_DATA, 7, "payload");
    buf[12] ^= 0x10; /* one bit in the payload */
    TEST_ASSERT_EQUAL_INT(RDT_ERR_CHECKSUM, rdt_decode(buf, n, &out));
}

void test_decode_null_arguments(void)
{
    uint8_t buf[RDT_MAX_PACKET];
    struct rdt_packet out;
    size_t n = make_packet(buf, RDT_ACK, 1, NULL);
    TEST_ASSERT_EQUAL_INT(RDT_ERR_ARG, rdt_decode(NULL, n, &out));
    TEST_ASSERT_EQUAL_INT(RDT_ERR_ARG, rdt_decode(buf, n, NULL));
}

/* ======================================================================= layer 2: sender */

void test_sender_init_rejects_bad_arguments(void)
{
    struct rdt_sender s;
    const uint8_t data[] = { 1 };
    TEST_ASSERT_EQUAL_INT(-1, rdt_sender_init(NULL, data, 1, 8, 250));
    TEST_ASSERT_EQUAL_INT(-1, rdt_sender_init(&s, NULL, 1, 8, 250));
    TEST_ASSERT_EQUAL_INT(-1, rdt_sender_init(&s, data, (size_t)RDT_MAX_FILE + 1, 8, 250));
    TEST_ASSERT_EQUAL_INT(-1, rdt_sender_init(&s, data, 1, 0, 250));
    TEST_ASSERT_EQUAL_INT(-1, rdt_sender_init(&s, data, 1, RDT_MAX_WINDOW + 1, 250));
    TEST_ASSERT_EQUAL_INT(-1, rdt_sender_init(&s, data, 1, 8, 0));
    rdt_sender_destroy(&s); /* safe after a failed init */
    rdt_sender_destroy(NULL);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 1, RDT_MAX_WINDOW, 250));
    rdt_sender_destroy(&s);
    rdt_sender_destroy(&s); /* and twice */
}

void test_sender_null_arguments_and_failed_init(void)
{
    struct rdt_sender s;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    size_t n = make_packet(buf, RDT_ACK, 1, NULL);
    rdt_sender_start(NULL, 0, &act);
    rdt_sender_on_datagram(NULL, buf, n, 0, &act);
    rdt_sender_on_timeout(NULL, 0, &act);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, NULL, 0, 1, 250));
    rdt_sender_start(&s, 0, NULL);
    rdt_sender_on_datagram(&s, buf, n, 0, NULL);
    rdt_sender_on_timeout(&s, 0, NULL);
    rdt_sender_destroy(&s);

    /* A sender whose init failed sends nothing when started. */
    TEST_ASSERT_EQUAL_INT(-1, rdt_sender_init(&s, NULL, 5, 1, 250));
    rdt_sender_start(&s, 0, &act);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)act.ndgram);
    TEST_ASSERT_FALSE(act.timer_set);
    rdt_sender_destroy(&s);
}

void test_sender_cuts_2500_bytes_into_three_packets_and_a_fin(void)
{
    /* The handout's Example 1. */
    uint8_t *data = make_file(2500, 9);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 2500, 8, 250));
    rdt_sender_start(&s, 1000, &act);
    assert_sends(&act, 0, 3);
    TEST_ASSERT_EQUAL_UINT16(1024, action_packet(&act, 0).len);
    TEST_ASSERT_EQUAL_UINT16(1024, action_packet(&act, 1).len);
    TEST_ASSERT_EQUAL_UINT16(452, action_packet(&act, 2).len);
    TEST_ASSERT_EQUAL_MEMORY(data + 2048, action_packet(&act, 2).payload, 452);
    TEST_ASSERT_TRUE(act.timer_set);
    TEST_ASSERT_TRUE(act.timer_due_ms == 1250);

    ack_sender(&s, 3, 1100, &act); /* all DATA acknowledged: now the FIN */
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)act.ndgram);
    TEST_ASSERT_EQUAL_UINT8(RDT_FIN, action_packet(&act, 0).type);
    TEST_ASSERT_EQUAL_UINT32(3, action_packet(&act, 0).seq);
    TEST_ASSERT_EQUAL_INT(RDT_RUNNING, act.result);

    ack_sender(&s, 4, 1200, &act); /* the FIN's ACK */
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)act.ndgram);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, act.result);
    TEST_ASSERT_FALSE(act.timer_set);
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_empty_file_is_a_single_fin(void)
{
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, NULL, 0, 8, 250));
    rdt_sender_start(&s, 0, &act);
    TEST_ASSERT_EQUAL_UINT(1, (unsigned)act.ndgram);
    TEST_ASSERT_EQUAL_UINT8(RDT_FIN, action_packet(&act, 0).type);
    TEST_ASSERT_EQUAL_UINT32(0, action_packet(&act, 0).seq);
    ack_sender(&s, 1, 10, &act);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, act.result);
    rdt_sender_destroy(&s);
}

void test_sender_exact_multiple_of_1024(void)
{
    uint8_t *data = make_file(3072, 4);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 3072, 8, 250));
    rdt_sender_start(&s, 0, &act);
    assert_sends(&act, 0, 3); /* no short, empty fourth packet */
    TEST_ASSERT_EQUAL_UINT16(1024, action_packet(&act, 2).len);
    ack_sender(&s, 3, 10, &act);
    TEST_ASSERT_EQUAL_UINT8(RDT_FIN, action_packet(&act, 0).type);
    TEST_ASSERT_EQUAL_UINT32(3, action_packet(&act, 0).seq);
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_window_full_waits_for_an_ack(void)
{
    uint8_t *data = make_file(10 * 1024, 1);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 10 * 1024, 4, 250));
    rdt_sender_start(&s, 0, &act);
    assert_sends(&act, 0, 4); /* next reached base + N */
    rdt_sender_start(&s, 1, &act);
    assert_sends(&act, 0, 0); /* still full */
    ack_sender(&s, 1, 5, &act);
    assert_sends(&act, 4, 1); /* one ACK, one slot, one packet */
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_cumulative_ack_slides_several_packets(void)
{
    uint8_t *data = make_file(10 * 1024, 2);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 10 * 1024, 4, 250));
    rdt_sender_start(&s, 0, &act);
    ack_sender(&s, 3, 100, &act); /* acknowledges 0, 1 and 2 at once */
    TEST_ASSERT_EQUAL_UINT32(3, s.base);
    assert_sends(&act, 4, 3);
    TEST_ASSERT_TRUE(act.timer_due_ms == 350); /* restarted */
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_duplicate_ack_changes_nothing(void)
{
    uint8_t *data = make_file(10 * 1024, 3);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 10 * 1024, 4, 250));
    rdt_sender_start(&s, 0, &act);
    ack_sender(&s, 2, 100, &act);
    TEST_ASSERT_TRUE(act.timer_due_ms == 350);
    ack_sender(&s, 2, 200, &act); /* duplicate: seq == base */
    assert_sends(&act, 0, 0);
    TEST_ASSERT_TRUE(act.timer_due_ms == 350); /* the timer was NOT restarted */
    ack_sender(&s, 1, 210, &act); /* older still: seq < base */
    assert_sends(&act, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(2, s.base);
    TEST_ASSERT_EQUAL_UINT(0, s.fruitless);
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_ignores_ack_beyond_next_damage_and_non_acks(void)
{
    uint8_t *data = make_file(4 * 1024, 6);
    uint8_t buf[RDT_MAX_PACKET];
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 4 * 1024, 2, 250));
    rdt_sender_start(&s, 0, &act);
    ack_sender(&s, 3, 10, &act); /* next is 2: nothing was ever sent as packet 2 */
    assert_sends(&act, 0, 0);
    size_t n = make_packet(buf, RDT_ACK, 1, NULL);
    buf[5] ^= 0x01; /* a corrupted ACK is a lost ACK */
    rdt_sender_on_datagram(&s, buf, n, 10, &act);
    assert_sends(&act, 0, 0);
    n = make_packet(buf, RDT_DATA, 1, "z"); /* not an ACK */
    rdt_sender_on_datagram(&s, buf, n, 10, &act);
    assert_sends(&act, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(0, s.base);
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_lost_acks_cost_nothing(void)
{
    /* The handout's Example 3: base 2, next 5, ACK 3 and ACK 4 lost, ACK 5 arrives. */
    uint8_t *data = make_file(6 * 1024, 7);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 6 * 1024, 3, 250));
    rdt_sender_start(&s, 0, &act);
    ack_sender(&s, 2, 10, &act);
    assert_sends(&act, 3, 2);
    TEST_ASSERT_EQUAL_UINT32(2, s.base);
    TEST_ASSERT_EQUAL_UINT32(5, s.next);
    ack_sender(&s, 5, 20, &act);
    TEST_ASSERT_EQUAL_UINT32(5, s.base); /* one step */
    assert_sends(&act, 5, 1);            /* only the new packet; 2, 3, 4 are not resent */
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_timeout_resends_the_whole_window(void)
{
    uint8_t *data = make_file(10 * 1024, 8);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 10 * 1024, 4, 250));
    rdt_sender_start(&s, 0, &act);
    rdt_sender_on_timeout(&s, 249, &act); /* not yet */
    assert_sends(&act, 0, 0);
    rdt_sender_on_timeout(&s, 250, &act);
    assert_sends(&act, 0, 4); /* go back to base and resend all of it */
    TEST_ASSERT_TRUE(act.timer_due_ms == 500);
    TEST_ASSERT_EQUAL_UINT(1, s.fruitless);
    TEST_ASSERT_EQUAL_MEMORY(data + 3 * 1024, action_packet(&act, 3).payload, 1024);
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_trace_window_4_six_packets(void)
{
    /* Window 4, six DATA packets. ACK 1 slides one; ACK 3 (ACK 2 lost) slides two; ACK 3
     * again is a duplicate; the timeout goes back to base 3. */
    uint8_t *data = make_file(6 * 1024, 10);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 6 * 1024, 4, 250));
    rdt_sender_start(&s, 0, &act);
    assert_sends(&act, 0, 4);
    ack_sender(&s, 1, 50, &act);
    assert_sends(&act, 4, 1);
    ack_sender(&s, 3, 60, &act);
    assert_sends(&act, 5, 1); /* only 5 is left; the FIN waits for every DATA ACK */
    ack_sender(&s, 3, 70, &act);
    assert_sends(&act, 0, 0);
    rdt_sender_on_timeout(&s, 310, &act);
    assert_sends(&act, 3, 3);
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_gives_up_after_10_timeouts(void)
{
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, NULL, 0, 8, 100));
    rdt_sender_start(&s, 0, &act);
    uint64_t now = 0;
    for (unsigned i = 1; i < RDT_MAX_TIMEOUTS; i++) {
        now += 100;
        rdt_sender_on_timeout(&s, now, &act);
        TEST_ASSERT_EQUAL_INT(RDT_RUNNING, act.result);
        assert_sends(&act, 0, 1); /* the FIN, again */
    }
    now += 100;
    rdt_sender_on_timeout(&s, now, &act); /* the 10th in a row */
    TEST_ASSERT_EQUAL_INT(RDT_GAVE_UP, act.result);
    assert_sends(&act, 0, 0);
    TEST_ASSERT_FALSE(act.timer_set);
    ack_sender(&s, 1, now + 1, &act); /* too late: a finished sender ignores everything */
    TEST_ASSERT_EQUAL_INT(RDT_GAVE_UP, act.result);
    rdt_sender_start(&s, now + 2, &act);
    TEST_ASSERT_EQUAL_INT(RDT_GAVE_UP, act.result);
    assert_sends(&act, 0, 0);
    rdt_sender_destroy(&s);
}

void test_sender_progress_resets_the_give_up_count(void)
{
    uint8_t *data = make_file(2 * 1024, 11);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 2 * 1024, 1, 100));
    rdt_sender_start(&s, 0, &act);
    uint64_t now = 0;
    for (unsigned i = 1; i < RDT_MAX_TIMEOUTS; i++) {
        now += 100;
        rdt_sender_on_timeout(&s, now, &act);
    }
    TEST_ASSERT_EQUAL_UINT(9, s.fruitless);
    ack_sender(&s, 1, now, &act); /* progress */
    TEST_ASSERT_EQUAL_UINT(0, s.fruitless);
    for (unsigned i = 1; i < RDT_MAX_TIMEOUTS; i++) {
        now += 100;
        rdt_sender_on_timeout(&s, now, &act);
        TEST_ASSERT_EQUAL_INT(RDT_RUNNING, act.result);
    }
    rdt_sender_destroy(&s);
    free(data);
}

void test_sender_window_of_one_is_stop_and_wait(void)
{
    uint8_t *data = make_file(3 * 1024, 12);
    struct rdt_sender s;
    struct rdt_actions act;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 3 * 1024, 1, 250));
    rdt_sender_start(&s, 0, &act);
    assert_sends(&act, 0, 1);
    ack_sender(&s, 1, 1, &act);
    assert_sends(&act, 1, 1);
    rdt_sender_on_timeout(&s, 251, &act);
    assert_sends(&act, 1, 1);
    rdt_sender_destroy(&s);
    free(data);
}

/* ===================================================================== layer 2: receiver */

void test_receiver_delivers_in_order(void)
{
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    rdt_receiver_init(&r, 0, 30000, 2000);
    size_t n = make_packet(buf, RDT_DATA, 0, "ab");
    rdt_receiver_on_datagram(&r, buf, n, 1, &act);
    TEST_ASSERT_EQUAL_UINT(2, (unsigned)act.deliver_len);
    TEST_ASSERT_EQUAL_MEMORY("ab", act.deliver, 2);
    TEST_ASSERT_EQUAL_UINT32(1, action_packet(&act, 0).seq); /* ACK 1 */
    TEST_ASSERT_EQUAL_UINT8(RDT_ACK, action_packet(&act, 0).type);
    n = make_packet(buf, RDT_DATA, 1, "c");
    rdt_receiver_on_datagram(&r, buf, n, 2, &act);
    TEST_ASSERT_EQUAL_UINT32(2, action_packet(&act, 0).seq);
}

void test_receiver_reacks_a_duplicate_without_delivering(void)
{
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    rdt_receiver_init(&r, 0, 30000, 2000);
    size_t n = make_packet(buf, RDT_DATA, 0, "ab");
    rdt_receiver_on_datagram(&r, buf, n, 1, &act);
    rdt_receiver_on_datagram(&r, buf, n, 2, &act);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)act.deliver_len);
    TEST_ASSERT_EQUAL_UINT32(1, action_packet(&act, 0).seq); /* ACK 1 again */
    TEST_ASSERT_EQUAL_UINT32(1, r.expected);
}

void test_receiver_discards_a_packet_from_beyond_a_gap(void)
{
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    rdt_receiver_init(&r, 0, 30000, 2000);
    size_t n = make_packet(buf, RDT_DATA, 1, "late"); /* 0 is missing */
    rdt_receiver_on_datagram(&r, buf, n, 1, &act);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)act.deliver_len); /* not buffered either */
    TEST_ASSERT_EQUAL_UINT32(0, action_packet(&act, 0).seq);
    TEST_ASSERT_EQUAL_UINT32(0, r.expected);
}

void test_receiver_sends_nothing_for_damaged_packets_or_acks(void)
{
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    rdt_receiver_init(&r, 0, 30000, 2000);
    size_t n = make_packet(buf, RDT_DATA, 0, "ab");
    buf[11] ^= 0x80;
    rdt_receiver_on_datagram(&r, buf, n, 1, &act);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)act.ndgram); /* no NAK, no re-ACK: silence */
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)act.deliver_len);
    n = make_packet(buf, RDT_ACK, 0, NULL);
    rdt_receiver_on_datagram(&r, buf, n, 1, &act);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)act.ndgram);
}

void test_receiver_fin_linger_and_repeated_fin(void)
{
    /* The handout's Example 5: the ACK for the FIN is lost, so the FIN comes again. */
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    rdt_receiver_init(&r, 0, 30000, 2000);
    size_t n = make_packet(buf, RDT_DATA, 0, "x");
    rdt_receiver_on_datagram(&r, buf, n, 1, &act);
    n = make_packet(buf, RDT_FIN, 1, NULL);
    rdt_receiver_on_datagram(&r, buf, n, 100, &act);
    TEST_ASSERT_TRUE(act.fin);
    TEST_ASSERT_EQUAL_UINT32(2, action_packet(&act, 0).seq);
    TEST_ASSERT_TRUE(act.timer_due_ms == 2100); /* lingering until now + 2 s */
    rdt_receiver_on_datagram(&r, buf, n, 300, &act); /* the repeated FIN */
    TEST_ASSERT_FALSE(act.fin);
    TEST_ASSERT_EQUAL_UINT32(2, action_packet(&act, 0).seq); /* the same ACK */
    TEST_ASSERT_TRUE(act.timer_due_ms == 2100);               /* the linger is not extended */
    rdt_receiver_on_timeout(&r, 2099, &act);
    TEST_ASSERT_EQUAL_INT(RDT_RUNNING, act.result);
    rdt_receiver_on_timeout(&r, 2100, &act);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, act.result);
    TEST_ASSERT_FALSE(act.timer_set);
    rdt_receiver_on_datagram(&r, buf, n, 2200, &act); /* after exit, nothing */
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)act.ndgram);
}

void test_receiver_fin_from_beyond_a_gap_is_reacked(void)
{
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    rdt_receiver_init(&r, 0, 30000, 2000);
    size_t n = make_packet(buf, RDT_FIN, 3, NULL);
    rdt_receiver_on_datagram(&r, buf, n, 1, &act);
    TEST_ASSERT_FALSE(act.fin);
    TEST_ASSERT_FALSE(r.finished);
    TEST_ASSERT_EQUAL_UINT32(0, action_packet(&act, 0).seq);
}

void test_receiver_gives_up_after_30_idle_seconds(void)
{
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    rdt_receiver_init(&r, 0, 30000, 2000);
    rdt_receiver_on_timeout(&r, 29999, &act);
    TEST_ASSERT_EQUAL_INT(RDT_RUNNING, act.result);
    TEST_ASSERT_TRUE(act.timer_due_ms == 30000);
    size_t n = make_packet(buf, RDT_DATA, 5, "?"); /* valid, even if unwanted */
    rdt_receiver_on_datagram(&r, buf, n, 10000, &act);
    TEST_ASSERT_TRUE(act.timer_due_ms == 40000); /* the clock restarted */
    rdt_receiver_on_timeout(&r, 39999, &act);
    TEST_ASSERT_EQUAL_INT(RDT_RUNNING, act.result);
    rdt_receiver_on_timeout(&r, 40000, &act);
    TEST_ASSERT_EQUAL_INT(RDT_GAVE_UP, act.result);
}

void test_receiver_trace_with_damage_and_duplicates(void)
{
    /* Expecting packet 2 of four, FIN 4. Arrivals and the ACK each one earns (-1 = none):
     * DATA 3 -> 2, DATA 2 flipped -> none, DATA 2 -> 3, DATA 2 again -> 3, DATA 3 -> 4,
     * FIN 4 -> 5, FIN 4 again -> 5. */
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    rdt_receiver_init(&r, 0, 30000, 2000);
    r.expected = 2;
    const uint8_t types[] = { RDT_DATA, RDT_DATA, RDT_DATA, RDT_DATA, RDT_DATA, RDT_FIN, RDT_FIN };
    const uint32_t seqs[] = { 3, 2, 2, 2, 3, 4, 4 };
    const long acks[] = { 2, -1, 3, 3, 4, 5, 5 };
    const size_t delivered[] = { 0, 0, 1, 0, 1, 0, 0 };
    for (size_t i = 0; i < 7; i++) {
        size_t n = make_packet(buf, types[i], seqs[i], types[i] == RDT_DATA ? "d" : NULL);
        if (i == 1) {
            buf[3] ^= 0x04;
        }
        rdt_receiver_on_datagram(&r, buf, n, i, &act);
        long ack = act.ndgram == 0 ? -1 : (long)action_packet(&act, 0).seq;
        TEST_ASSERT_EQUAL_INT((int)acks[i], (int)ack);
        TEST_ASSERT_EQUAL_UINT((unsigned)delivered[i], (unsigned)act.deliver_len);
    }
    TEST_ASSERT_TRUE(r.finished);
}

void test_receiver_null_arguments(void)
{
    struct rdt_receiver r;
    struct rdt_actions act;
    uint8_t buf[RDT_MAX_PACKET];
    size_t n = make_packet(buf, RDT_DATA, 0, "a");
    rdt_receiver_init(NULL, 0, 1, 1);
    rdt_receiver_init(&r, 0, 1, 1);
    rdt_receiver_on_datagram(NULL, buf, n, 0, &act);
    rdt_receiver_on_datagram(&r, buf, n, 0, NULL);
    rdt_receiver_on_timeout(NULL, 0, &act);
    rdt_receiver_on_timeout(&r, 0, NULL);
    TEST_ASSERT_EQUAL_UINT32(0, r.expected);
}

/* ===================================== layer 2: the two state machines, end to end */

/* The handout's Go-Back-N timeline exactly: window 4, six DATA packets, DATA 2 lost. */
void test_timeline_from_the_handout(void)
{
    uint8_t *data = make_file(6 * 1024, 13);
    struct rdt_sender s;
    struct rdt_receiver r;
    struct rdt_actions out;
    struct rdt_actions in;
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 6 * 1024, 4, 250));
    rdt_receiver_init(&r, 0, 30000, 2000);

    rdt_sender_start(&s, 0, &out); /* DATA 0, 1, 2, 3 */
    struct rdt_actions first = out;
    TEST_ASSERT_EQUAL_INT(1, (int)feed_receiver(&r, &first, 0, 1, &in)); /* ACK 1 */
    ack_sender(&s, 1, 2, &out);
    struct rdt_actions sent4 = out; /* DATA 4 */
    assert_sends(&sent4, 4, 1);
    TEST_ASSERT_EQUAL_INT(2, (int)feed_receiver(&r, &first, 1, 3, &in)); /* ACK 2 */
    ack_sender(&s, 2, 4, &out);
    struct rdt_actions sent5 = out; /* DATA 5 */
    assert_sends(&sent5, 5, 1);
    /* DATA 2 is lost. 3, 4 and 5 arrive out of order and each earns a duplicate ACK 2. */
    TEST_ASSERT_EQUAL_INT(2, (int)feed_receiver(&r, &first, 3, 5, &in));
    ack_sender(&s, 2, 6, &out);
    assert_sends(&out, 0, 0);
    TEST_ASSERT_EQUAL_INT(2, (int)feed_receiver(&r, &sent4, 0, 7, &in));
    TEST_ASSERT_EQUAL_INT(2, (int)feed_receiver(&r, &sent5, 0, 8, &in));
    ack_sender(&s, 2, 9, &out);
    assert_sends(&out, 0, 0);
    /* The timer for DATA 2 runs out: go back and resend 2 to 5. */
    rdt_sender_on_timeout(&s, 254, &out);
    struct rdt_actions resent = out;
    assert_sends(&resent, 2, 4);
    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT((int)i + 3, (int)feed_receiver(&r, &resent, i, 300, &in));
        ack_sender(&s, (uint32_t)i + 3, 301, &out);
    }
    TEST_ASSERT_EQUAL_UINT32(6, s.base); /* ACK 6: all DATA acknowledged, FIN on its way */
    TEST_ASSERT_EQUAL_UINT8(RDT_FIN, action_packet(&out, 0).type);
    rdt_sender_destroy(&s);
    free(data);
}

/* An in-memory channel that damages datagrams the way the relay does. */
#define CHAN_CAP 1024

struct chan_msg {
    uint8_t bytes[RDT_MAX_PACKET];
    size_t len;
};

struct channel {
    struct chan_msg *msgs; /* a ring of CHAN_CAP */
    size_t head;
    size_t count;
    double rate;    /* the same rate for loss, corruption and duplication */
    struct rng *rng;
    unsigned damaged;
};

static void chan_init(struct channel *c, struct rng *rng, double rate)
{
    memset(c, 0, sizeof *c);
    c->msgs = calloc(CHAN_CAP, sizeof *c->msgs);
    TEST_ASSERT_NOT_NULL(c->msgs);
    c->rate = rate;
    c->rng = rng;
}

static struct chan_msg *chan_append(struct channel *c, const uint8_t *bytes, size_t len)
{
    TEST_ASSERT_TRUE(c->count < CHAN_CAP);
    struct chan_msg *m = &c->msgs[(c->head + c->count) % CHAN_CAP];
    memcpy(m->bytes, bytes, len);
    m->len = len;
    c->count++;
    return m;
}

/* The relay's order: drop; otherwise flip one random bit; otherwise deliver twice. */
static void chan_push(struct channel *c, const uint8_t *bytes, size_t len)
{
    if (rng_unit(c->rng) < c->rate) {
        c->damaged++;
        return;
    }
    struct chan_msg *m = chan_append(c, bytes, len);
    if (rng_unit(c->rng) < c->rate) {
        uint32_t bit = rng_next(c->rng) % (uint32_t)(len * 8);
        m->bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        c->damaged++;
    } else if (rng_unit(c->rng) < c->rate) {
        chan_append(c, bytes, len);
        c->damaged++;
    }
}

static void chan_push_all(struct channel *c, const struct rdt_actions *act)
{
    for (size_t i = 0; i < act->ndgram; i++) {
        chan_push(c, act->dgram[i], act->dgram_len[i]);
    }
}

static bool chan_pop(struct channel *c, struct chan_msg *out)
{
    if (c->count == 0) {
        return false;
    }
    *out = c->msgs[c->head];
    c->head = (c->head + 1) % CHAN_CAP;
    c->count--;
    return true;
}

struct sim_result {
    enum rdt_result sender;   /* how the sender finished */
    enum rdt_result receiver; /* how the receiver finished, after its linger */
    bool identical;           /* the bytes delivered are exactly the bytes sent */
    unsigned damaged;         /* datagrams dropped, corrupted or duplicated, both ways */
};

/* Runs a whole transfer between the two state machines over two damaging channels. Time
 * stands still while anything is in flight and jumps to the sender's timer when nothing is,
 * so a timeout fires only when a loss has really stalled the transfer. */
static struct sim_result simulate(const uint8_t *data, size_t size, uint32_t window, double rate,
                                  uint32_t seed)
{
    struct rng rng = { seed };
    struct channel down;
    struct channel up;
    struct rdt_sender s;
    struct rdt_receiver r;
    struct rdt_actions act;
    struct chan_msg m;
    struct sim_result res;
    uint8_t *got = malloc(size + 1);
    size_t delivered = 0;
    uint64_t now = 0;

    TEST_ASSERT_NOT_NULL(got);
    chan_init(&down, &rng, rate);
    chan_init(&up, &rng, rate);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, size, window, 250));
    rdt_receiver_init(&r, now, 30000, 2000);
    rdt_sender_start(&s, now, &act);
    chan_push_all(&down, &act);

    for (long i = 0; i < 5000000 && s.state == RDT_RUNNING; i++) {
        if (chan_pop(&down, &m)) {
            rdt_receiver_on_datagram(&r, m.bytes, m.len, now, &act);
            if (act.deliver_len > 0) {
                TEST_ASSERT_TRUE(delivered + act.deliver_len <= size);
                memcpy(got + delivered, act.deliver, act.deliver_len);
                delivered += act.deliver_len;
            }
            chan_push_all(&up, &act);
        } else if (chan_pop(&up, &m)) {
            rdt_sender_on_datagram(&s, m.bytes, m.len, now, &act);
            chan_push_all(&down, &act);
        } else {
            TEST_ASSERT_TRUE(s.timer_running);
            now = s.timer_due_ms;
            rdt_sender_on_timeout(&s, now, &act);
            chan_push_all(&down, &act);
        }
    }
    rdt_receiver_on_timeout(&r, now + 2000, &act);

    res.sender = s.state;
    res.receiver = r.state;
    res.identical = delivered == size && (size == 0 || memcmp(got, data, size) == 0);
    res.damaged = down.damaged + up.damaged;
    rdt_sender_destroy(&s);
    free(down.msgs);
    free(up.msgs);
    free(got);
    return res;
}

void test_transfer_clean_channel_various_sizes_and_windows(void)
{
    const size_t sizes[] = { 0, 1, 1023, 1024, 1025, 5 * 1024, 20000 };
    const uint32_t windows[] = { 1, 4, 16, 64 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint8_t *data = make_file(sizes[i], 21);
        for (size_t w = 0; w < sizeof windows / sizeof windows[0]; w++) {
            struct sim_result res = simulate(data, sizes[i], windows[w], 0.0, 1);
            TEST_ASSERT_EQUAL_INT(RDT_DONE, res.sender);
            TEST_ASSERT_EQUAL_INT(RDT_DONE, res.receiver);
            TEST_ASSERT_TRUE(res.identical);
        }
        free(data);
    }
}

void test_transfer_lossy_20_percent_each_way_several_seeds(void)
{
    /* The test that says the protocol works: 20% loss, 20% corruption and 20% duplication,
     * in both directions, for eight fixed seeds. About 50 packets, the last one short. */
    const size_t size = 50 * 1024 + 300;
    uint8_t *data = make_file(size, 42);
    char msg[64];
    for (uint32_t seed = 1; seed <= 8; seed++) {
        snprintf(msg, sizeof msg, "seed %u", (unsigned)seed);
        struct sim_result res = simulate(data, size, 8, 0.2, seed);
        TEST_ASSERT_EQUAL_INT_MESSAGE(RDT_DONE, res.sender, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(RDT_DONE, res.receiver, msg);
        TEST_ASSERT_TRUE_MESSAGE(res.identical, msg);
        TEST_ASSERT_TRUE_MESSAGE(res.damaged > 50, msg); /* the channel really was hostile */
    }
    free(data);
}

void test_transfer_lossy_stop_and_wait(void)
{
    /* Window 1 is rdt3.0. Same channel, gentler rate, since every loss stalls everything. */
    const size_t size = 20 * 1024 + 17;
    uint8_t *data = make_file(size, 43);
    for (uint32_t seed = 1; seed <= 4; seed++) {
        struct sim_result res = simulate(data, size, 1, 0.1, seed);
        TEST_ASSERT_EQUAL_INT(RDT_DONE, res.sender);
        TEST_ASSERT_TRUE(res.identical);
    }
    free(data);
}

/* ===================================================================== layer 3: parsing */

/* Parses a command line given as a NULL-terminated list of words. */
static int parse(struct rdt_config *cfg, char *err, const char *words[])
{
    char *argv[32];
    int argc = 0;
    while (words[argc] != NULL) {
        argv[argc] = (char *)words[argc];
        argc++;
    }
    argv[argc] = NULL;
    err[0] = '\0';
    return rdt_parse_args(argc, argv, cfg, err, 128);
}

void test_parse_args_send_with_every_option(void)
{
    struct rdt_config cfg;
    char err[128];
    const char *words[] = { "myapp", "send", "-s", "jdoe-1", "-w", "16", "-T", "300", "-l",
                            "0.1", "-c", ".05", "-d", "0", "-p", "4251", "127.0.0.1", "in.bin",
                            NULL };
    TEST_ASSERT_EQUAL_INT(0, parse(&cfg, err, words));
    TEST_ASSERT_EQUAL_INT(RDT_MODE_SEND, cfg.mode);
    TEST_ASSERT_EQUAL_STRING("jdoe-1", cfg.session);
    TEST_ASSERT_EQUAL_UINT32(16, cfg.window);
    TEST_ASSERT_EQUAL_UINT32(300, cfg.timeout_ms);
    TEST_ASSERT_EQUAL_STRING("0.1", cfg.loss);
    TEST_ASSERT_EQUAL_STRING(".05", cfg.corrupt);
    TEST_ASSERT_EQUAL_STRING("0", cfg.dup);
    TEST_ASSERT_EQUAL_STRING("4251", cfg.port);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", cfg.relay);
    TEST_ASSERT_EQUAL_STRING("in.bin", cfg.file);
}

void test_parse_args_recv_defaults(void)
{
    struct rdt_config cfg;
    char err[128];
    const char *words[] = { "myapp", "recv", "-s", "a", "localhost", "out.bin", NULL };
    TEST_ASSERT_EQUAL_INT(0, parse(&cfg, err, words));
    TEST_ASSERT_EQUAL_INT(RDT_MODE_RECV, cfg.mode);
    TEST_ASSERT_EQUAL_UINT32(RDT_DEFAULT_WINDOW, cfg.window);
    TEST_ASSERT_EQUAL_UINT32(RDT_DEFAULT_TIMEOUT_MS, cfg.timeout_ms);
    TEST_ASSERT_EQUAL_STRING("0", cfg.loss);
    TEST_ASSERT_EQUAL_STRING(RDT_DEFAULT_PORT, cfg.port);
    TEST_ASSERT_EQUAL_STRING("localhost", cfg.relay);
}

void test_parse_args_rejects_wrong_command_lines(void)
{
    struct rdt_config cfg;
    char err[128];
    const char *bad[][12] = {
        { "myapp", "fly", "-s", "a", "h", "f", NULL },              /* unknown mode */
        { "myapp", "send", NULL },                                  /* nothing else */
        { "myapp", "send", "-s", "a", "h", NULL },                  /* no file */
        { "myapp", "send", "-s", "a", "h", "f", "g", NULL },        /* one word too many */
        { "myapp", "send", "h", "f", NULL },                        /* no -s */
        { "myapp", "send", "-s", "Upper", "h", "f", NULL },         /* bad session */
        { "myapp", "send", "-s", "abcdefghijklmnopqrstuvwxyz0123456", "h", "f", NULL },
        { "myapp", "send", "-s", "", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-w", "0", "h", "f", NULL },  /* window range */
        { "myapp", "send", "-s", "a", "-w", "65", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-w", "8x", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-w", "", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-T", "0", "h", "f", NULL },  /* timeout range */
        { "myapp", "send", "-s", "a", "-l", "0.6", "h", "f", NULL },/* rates */
        { "myapp", "send", "-s", "a", "-l", "1e-05", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-c", "-0.1", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-d", "5.", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-d", "0.1.2", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-d", "", "h", "f", NULL },
        { "myapp", "send", "-s", "a", "-p", "0", "h", "f", NULL },  /* port range */
        { "myapp", "send", "-s", "a", "-p", "65536", "h", "f", NULL },
        { "myapp", "recv", "-s", "a", "-w", "4", "h", "f", NULL },  /* send-only option */
        { "myapp", "send", "-s", "a", "-x", "h", "f", NULL },       /* unknown option */
        { "myapp", "send", "h", "f", "-s", NULL },                  /* -s with no value */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char msg[32];
        snprintf(msg, sizeof msg, "case %u", (unsigned)i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, parse(&cfg, err, bad[i]), msg);
        TEST_ASSERT_TRUE_MESSAGE(strlen(err) > 0, msg); /* and says why */
    }
    /* A good command line still parses after all of that: getopt's state was reset. */
    const char *good[] = { "myapp", "send", "-s", "b", "-w", "64", "h", "f", NULL };
    TEST_ASSERT_EQUAL_INT(0, parse(&cfg, err, good));
    TEST_ASSERT_EQUAL_UINT32(64, cfg.window);
}

void test_parse_args_null_arguments(void)
{
    struct rdt_config cfg;
    char err[64];
    char *argv[] = { "myapp", "send", NULL };
    TEST_ASSERT_EQUAL_INT(-1, rdt_parse_args(1, argv, &cfg, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(-1, rdt_parse_args(2, NULL, &cfg, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(-1, rdt_parse_args(2, argv, NULL, err, sizeof err));
    TEST_ASSERT_EQUAL_INT(-1, rdt_parse_args(2, argv, &cfg, NULL, 0)); /* nowhere for a reason */
}

void test_usage_prints_the_interface(void)
{
    char text[2048];
    FILE *f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    rdt_usage(f);
    rewind(f);
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
    TEST_ASSERT_NOT_NULL(strstr(text, "Usage: myapp send -s <session> [-w window]"));
    TEST_ASSERT_NOT_NULL(strstr(text, "myapp recv -s <session> [-p port] <relay> <file>"));
    rdt_usage(NULL);
}

void test_hello_text(void)
{
    struct rdt_config cfg;
    char err[128];
    char hello[64];
    const char *recv_words[] = { "myapp", "recv", "-s", "jdoe-1", "h", "f", NULL };
    TEST_ASSERT_EQUAL_INT(0, parse(&cfg, err, recv_words));
    TEST_ASSERT_EQUAL_INT(17, rdt_hello_text(&cfg, hello, sizeof hello));
    TEST_ASSERT_EQUAL_STRING("HELLO jdoe-1 recv", hello);

    const char *send_words[] = { "myapp", "send", "-s", "jdoe-1", "-l", "0.1", "-c", "0.05",
                                 "h", "f", NULL };
    TEST_ASSERT_EQUAL_INT(0, parse(&cfg, err, send_words));
    TEST_ASSERT_EQUAL_INT(28, rdt_hello_text(&cfg, hello, sizeof hello));
    TEST_ASSERT_EQUAL_STRING("HELLO jdoe-1 send 0.1 0.05 0", hello);

    TEST_ASSERT_EQUAL_INT(-1, rdt_hello_text(&cfg, hello, 10)); /* does not fit */
    TEST_ASSERT_EQUAL_INT(-1, rdt_hello_text(NULL, hello, sizeof hello));
    TEST_ASSERT_EQUAL_INT(-1, rdt_hello_text(&cfg, NULL, sizeof hello));
    cfg.dup = NULL;
    TEST_ASSERT_EQUAL_INT(-1, rdt_hello_text(&cfg, hello, sizeof hello));
}

/* ============================================================== layer 3: clock and timer */

void test_now_ms_never_goes_backwards(void)
{
    uint64_t a = rdt_now_ms();
    usleep(2000);
    uint64_t b = rdt_now_ms();
    TEST_ASSERT_TRUE(b >= a + 1);
}

void test_poll_timeout(void)
{
    TEST_ASSERT_EQUAL_INT(-1, rdt_poll_timeout(false, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, rdt_poll_timeout(true, 100, 100));
    TEST_ASSERT_EQUAL_INT(0, rdt_poll_timeout(true, 100, 5000)); /* overdue: 0, not negative */
    TEST_ASSERT_EQUAL_INT(250, rdt_poll_timeout(true, 1250, 1000));
    TEST_ASSERT_EQUAL_INT(INT_MAX, rdt_poll_timeout(true, (uint64_t)1 << 40, 0));
}

/* ======================================================================= layer 3: sockets */

void test_open_resolves_names_and_reports_failures(void)
{
    char err[256];
    int fd = rdt_open("127.0.0.1", "4250", err, sizeof err);
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
    fd = rdt_open("localhost", "4250", err, sizeof err); /* a name, IPv4 only */
    TEST_ASSERT_TRUE(fd >= 0);
    close(fd);
    err[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-1, rdt_open("127.0.0.1", "no-such-service", err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "cannot resolve"));
    TEST_ASSERT_EQUAL_INT(-1, rdt_open(NULL, "4250", err, sizeof err));
    TEST_ASSERT_EQUAL_INT(-1, rdt_open("127.0.0.1", NULL, NULL, 0));
}

void test_wait_for_a_datagram(void)
{
    int a;
    int b;
    uint8_t buf[RDT_RECV_BUF];
    size_t n = 0;
    udp_pair(&a, &b);
    TEST_ASSERT_EQUAL_INT(0, rdt_wait(a, 0, buf, sizeof buf, &n)); /* nothing yet */
    TEST_ASSERT_EQUAL_INT(5, (int)send(b, "hello", 5, 0));
    TEST_ASSERT_EQUAL_INT(1, rdt_wait(a, 1000, buf, sizeof buf, &n));
    TEST_ASSERT_EQUAL_UINT(5, (unsigned)n);
    TEST_ASSERT_EQUAL_MEMORY("hello", buf, 5);
    TEST_ASSERT_EQUAL_INT(-1, rdt_wait(a, 0, NULL, sizeof buf, &n));
    TEST_ASSERT_EQUAL_INT(-1, rdt_wait(a, 0, buf, sizeof buf, NULL));
    close(a);
    close(b);
}

void test_wait_treats_port_unreachable_as_silence(void)
{
    char port[16];
    char err[128];
    uint8_t buf[RDT_RECV_BUF];
    size_t n = 0;
    dead_port(port, sizeof port);
    int fd = rdt_open("127.0.0.1", port, err, sizeof err);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(1, (int)send(fd, "x", 1, 0)); /* earns an ICMP port unreachable */
    TEST_ASSERT_EQUAL_INT(0, rdt_wait(fd, 200, buf, sizeof buf, &n));
    close(fd);
}

void test_wait_reports_a_broken_descriptor(void)
{
    int fds[2];
    uint8_t buf[RDT_RECV_BUF];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, pipe(fds));
    TEST_ASSERT_EQUAL_INT(1, (int)write(fds[1], "x", 1)); /* readable, but not a socket */
    TEST_ASSERT_EQUAL_INT(-1, rdt_wait(fds[0], 100, buf, sizeof buf, &n));
    close(fds[0]);
    close(fds[1]);
}

void test_send_actions(void)
{
    int a;
    int b;
    int fds[2];
    char port[16];
    char err[128];
    uint8_t buf[RDT_RECV_BUF];
    struct rdt_actions act;
    memset(&act, 0, sizeof act);
    act.ndgram = 2;
    act.dgram[0] = (const uint8_t *)"one";
    act.dgram_len[0] = 3;
    act.dgram[1] = (const uint8_t *)"two!";
    act.dgram_len[1] = 4;

    udp_pair(&a, &b);
    TEST_ASSERT_EQUAL_INT(0, rdt_send_actions(a, &act));
    TEST_ASSERT_EQUAL_UINT(3, (unsigned)recv_now(b, buf));
    TEST_ASSERT_EQUAL_UINT(4, (unsigned)recv_now(b, buf));
    TEST_ASSERT_EQUAL_INT(-1, rdt_send_actions(a, NULL));
    close(a);
    close(b);

    /* Nobody listening: the second send may see ECONNREFUSED, which counts as sent. */
    dead_port(port, sizeof port);
    int fd = rdt_open("127.0.0.1", port, err, sizeof err);
    TEST_ASSERT_EQUAL_INT(0, rdt_send_actions(fd, &act));
    TEST_ASSERT_EQUAL_INT(0, rdt_send_actions(fd, &act));
    close(fd);

    TEST_ASSERT_EQUAL_INT(0, pipe(fds)); /* not a socket at all */
    TEST_ASSERT_EQUAL_INT(-1, rdt_send_actions(fds[1], &act));
    close(fds[0]);
    close(fds[1]);
}

/* A fake relay: an unconnected socket, plus a client opened to it with rdt_open. */
struct fake_relay {
    int relay;
    int client;
    struct sockaddr_in client_addr;
};

static void fake_relay_open(struct fake_relay *f)
{
    struct sockaddr_in addr;
    char port[16];
    char err[128];
    socklen_t len = sizeof f->client_addr;
    f->relay = udp_bound(&addr);
    snprintf(port, sizeof port, "%u", (unsigned)ntohs(addr.sin_port));
    f->client = rdt_open("127.0.0.1", port, err, sizeof err);
    TEST_ASSERT_TRUE(f->client >= 0);
    /* Connecting fixed the client's port, so the relay can answer before it is asked. */
    TEST_ASSERT_EQUAL_INT(0, getsockname(f->client, (struct sockaddr *)&f->client_addr, &len));
}

static void fake_relay_say(struct fake_relay *f, const void *bytes, size_t len)
{
    TEST_ASSERT_EQUAL_INT((int)len, (int)sendto(f->relay, bytes, len, 0,
                                                (struct sockaddr *)&f->client_addr,
                                                sizeof f->client_addr));
}

static void fake_relay_close(struct fake_relay *f)
{
    close(f->relay);
    close(f->client);
}

void test_register_ok_and_exact_hello_bytes(void)
{
    struct fake_relay f;
    char reply[64];
    uint8_t buf[RDT_RECV_BUF];
    fake_relay_open(&f);
    fake_relay_say(&f, "OK", 2);
    TEST_ASSERT_EQUAL_INT(RDT_REG_OK,
                          rdt_register(f.client, "HELLO jdoe-1 recv", 5, 1000, reply, sizeof reply));
    TEST_ASSERT_EQUAL_UINT(17, (unsigned)recv_now(f.relay, buf)); /* no NUL, no newline */
    TEST_ASSERT_EQUAL_MEMORY("HELLO jdoe-1 recv", buf, 17);
    fake_relay_close(&f);
}

void test_register_reports_the_err_reason(void)
{
    struct fake_relay f;
    char reply[64];
    char tiny[4];
    fake_relay_open(&f);
    fake_relay_say(&f, "ERR no receiver", 15);
    TEST_ASSERT_EQUAL_INT(RDT_REG_REFUSED,
                          rdt_register(f.client, "HELLO a send 0 0 0", 5, 1000, reply, sizeof reply));
    TEST_ASSERT_EQUAL_STRING("no receiver", reply);
    fake_relay_say(&f, "ERR no receiver", 15);
    TEST_ASSERT_EQUAL_INT(RDT_REG_REFUSED,
                          rdt_register(f.client, "HELLO a send 0 0 0", 5, 1000, tiny, sizeof tiny));
    TEST_ASSERT_EQUAL_STRING("no ", tiny); /* cut to fit, still terminated */
    fake_relay_say(&f, "ERR", 3);
    TEST_ASSERT_EQUAL_INT(RDT_REG_REFUSED,
                          rdt_register(f.client, "HELLO a recv", 5, 1000, reply, sizeof reply));
    TEST_ASSERT_EQUAL_STRING("", reply);
    fake_relay_close(&f);
}

void test_register_retries_then_gives_up(void)
{
    struct fake_relay f;
    char reply[64];
    uint8_t buf[RDT_RECV_BUF];
    fake_relay_open(&f);
    TEST_ASSERT_EQUAL_INT(RDT_REG_NO_REPLY,
                          rdt_register(f.client, "HELLO a recv", 3, 5, reply, sizeof reply));
    unsigned hellos = 0;
    while (recv_now(f.relay, buf) > 0) {
        hellos++;
    }
    TEST_ASSERT_EQUAL_UINT(3, hellos); /* one per attempt */
    fake_relay_close(&f);
}

void test_register_ignores_stray_datagrams(void)
{
    struct fake_relay f;
    char reply[64];
    uint8_t packet[RDT_MAX_PACKET];
    size_t n = make_packet(packet, RDT_ACK, 1, NULL);
    fake_relay_open(&f);
    fake_relay_say(&f, packet, n);
    fake_relay_say(&f, "OKAY", 4);
    fake_relay_say(&f, "OK", 2);
    TEST_ASSERT_EQUAL_INT(RDT_REG_OK,
                          rdt_register(f.client, "HELLO a recv", 1, 1000, reply, sizeof reply));
    fake_relay_close(&f);
}

void test_register_with_no_relay_running(void)
{
    char port[16];
    char err[128];
    char reply[64];
    dead_port(port, sizeof port);
    int fd = rdt_open("127.0.0.1", port, err, sizeof err);
    TEST_ASSERT_EQUAL_INT(RDT_REG_NO_REPLY, rdt_register(fd, "HELLO a recv", 2, 20, reply,
                                                         sizeof reply));
    close(fd);
}

void test_register_bad_arguments_and_broken_socket(void)
{
    int fds[2];
    char reply[64];
    TEST_ASSERT_EQUAL_INT(RDT_REG_IO_ERROR, rdt_register(0, NULL, 1, 1, reply, sizeof reply));
    TEST_ASSERT_EQUAL_INT(RDT_REG_IO_ERROR, rdt_register(0, "HELLO", 1, 1, NULL, 8));
    TEST_ASSERT_EQUAL_INT(RDT_REG_IO_ERROR, rdt_register(0, "HELLO", 1, 1, reply, 0));
    TEST_ASSERT_EQUAL_INT(RDT_REG_IO_ERROR, rdt_register(0, "HELLO", 0, 1, reply, sizeof reply));
    TEST_ASSERT_EQUAL_INT(RDT_REG_IO_ERROR, rdt_register(0, "HELLO", 1, -1, reply, sizeof reply));
    TEST_ASSERT_EQUAL_INT(0, pipe(fds));
    TEST_ASSERT_EQUAL_INT(RDT_REG_IO_ERROR, rdt_register(fds[1], "HELLO a recv", 1, 1, reply,
                                                         sizeof reply));
    close(fds[0]);
    close(fds[1]);
}

/* ========================================================================= layer 3: files */

void test_read_file(void)
{
    char path[64];
    uint8_t *data = NULL;
    size_t size = 0;
    temp_path(path, sizeof path);

    TEST_ASSERT_EQUAL_INT(0, rdt_read_file(path, &data, &size)); /* empty */
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)size);
    free(data);

    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("some bytes", f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT(0, rdt_read_file(path, &data, &size));
    TEST_ASSERT_EQUAL_UINT(10, (unsigned)size);
    TEST_ASSERT_EQUAL_MEMORY("some bytes", data, 10);
    free(data);

    /* One byte over 16 MiB. Sparse, so nothing is actually written. */
    int fd = open(path, O_WRONLY);
    TEST_ASSERT_EQUAL_INT(0, ftruncate(fd, (off_t)RDT_MAX_FILE + 1));
    close(fd);
    TEST_ASSERT_EQUAL_INT(-1, rdt_read_file(path, &data, &size));
    TEST_ASSERT_EQUAL_INT(EFBIG, errno);
    unlink(path);

    TEST_ASSERT_EQUAL_INT(-1, rdt_read_file(path, &data, &size)); /* gone */
    TEST_ASSERT_EQUAL_INT(ENOENT, errno);
    TEST_ASSERT_EQUAL_INT(-1, rdt_read_file("/tmp", &data, &size));
    TEST_ASSERT_EQUAL_INT(EISDIR, errno);
    TEST_ASSERT_EQUAL_INT(-1, rdt_read_file("/dev/null", &data, &size));
    TEST_ASSERT_EQUAL_INT(EINVAL, errno);
    TEST_ASSERT_EQUAL_INT(-1, rdt_read_file(NULL, &data, &size));
    TEST_ASSERT_EQUAL_INT(-1, rdt_read_file(path, NULL, &size));
    TEST_ASSERT_EQUAL_INT(-1, rdt_read_file(path, &data, NULL));
}

/* ==================================================================== layer 3: event loops */

void test_sender_run_empty_file(void)
{
    /* The ACK is queued before the FIN is even sent; the sender cannot tell. */
    int a;
    int b;
    uint8_t buf[RDT_RECV_BUF];
    struct rdt_packet p;
    struct rdt_sender s;
    udp_pair(&a, &b);
    send_packet(b, RDT_ACK, 1, NULL);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, NULL, 0, 4, 1000));
    TEST_ASSERT_EQUAL_INT(RDT_DONE, rdt_sender_run(a, &s));
    size_t n = recv_now(b, buf);
    TEST_ASSERT_EQUAL_INT(RDT_OK, rdt_decode(buf, n, &p));
    TEST_ASSERT_EQUAL_UINT8(RDT_FIN, p.type);
    TEST_ASSERT_EQUAL_UINT32(0, p.seq);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, rdt_sender_step(a, &s)); /* finished stays finished */
    rdt_sender_destroy(&s);
    close(a);
    close(b);
}

void test_sender_run_gives_up_after_ten_fins(void)
{
    int a;
    int b;
    uint8_t buf[RDT_RECV_BUF];
    struct rdt_sender s;
    udp_pair(&a, &b);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, NULL, 0, 4, 1));
    TEST_ASSERT_EQUAL_INT(RDT_GAVE_UP, rdt_sender_run(a, &s));
    unsigned fins = 0;
    while (recv_now(b, buf) > 0) {
        fins++;
    }
    TEST_ASSERT_EQUAL_UINT(10, fins); /* the first one, then nine resends */
    rdt_sender_destroy(&s);
    close(a);
    close(b);
}

void test_sender_loop_errors(void)
{
    int fds[2];
    struct sockaddr_in addr;
    struct rdt_sender s;
    struct rdt_actions act;
    uint8_t *data = make_file(3 * 1024, 14);
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_sender_run(0, NULL));
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_sender_step(0, NULL));

    /* An unconnected socket cannot send without an address, so the first send fails. */
    int u = udp_bound(&addr);
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 3 * 1024, 1, 1000));
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_sender_step(u, &s)); /* never started */
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_sender_run(u, &s));
    rdt_sender_destroy(&s);

    /* An ACK arrives and slides the window; sending the next packet fails. */
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 3 * 1024, 1, 1000));
    rdt_sender_start(&s, rdt_now_ms(), &act);
    int peer = udp_bound(NULL);
    uint8_t ack[RDT_MAX_PACKET];
    size_t n = make_packet(ack, RDT_ACK, 1, NULL);
    TEST_ASSERT_EQUAL_INT((int)n, (int)sendto(peer, ack, n, 0, (struct sockaddr *)&addr,
                                              sizeof addr));
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_sender_step(u, &s));
    rdt_sender_destroy(&s);

    /* The timer has expired (started at time 0, long ago on the monotonic clock); resending
     * fails. */
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 3 * 1024, 1, 1000));
    rdt_sender_start(&s, 0, &act);
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_sender_step(u, &s));
    rdt_sender_destroy(&s);

    /* Readable, but not a socket: receiving fails. */
    TEST_ASSERT_EQUAL_INT(0, pipe(fds));
    TEST_ASSERT_EQUAL_INT(1, (int)write(fds[1], "x", 1));
    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, 3 * 1024, 1, 1000));
    rdt_sender_start(&s, rdt_now_ms(), &act);
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_sender_step(fds[0], &s));
    rdt_sender_destroy(&s);

    close(fds[0]);
    close(fds[1]);
    close(u);
    close(peer);
    free(data);
}

/* Reads a whole small file into buf and returns its size. */
static size_t slurp(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

void test_receiver_run_writes_the_file_and_closes_it(void)
{
    int a;
    int b;
    char path[64];
    uint8_t buf[RDT_RECV_BUF];
    struct rdt_packet p;
    struct rdt_receiver r;
    udp_pair(&a, &b);
    temp_path(path, sizeof path);
    FILE *out = fopen(path, "wb");
    send_packet(a, RDT_DATA, 0, "hello ");
    send_packet(a, RDT_DATA, 0, "hello "); /* a duplicate, written once */
    send_packet(a, RDT_DATA, 1, "world");
    send_packet(a, RDT_FIN, 2, NULL);
    rdt_receiver_init(&r, rdt_now_ms(), 2000, 5);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, rdt_receiver_run(b, &r, &out));
    TEST_ASSERT_NULL(out); /* closed at the FIN */
    TEST_ASSERT_EQUAL_UINT(11, (unsigned)slurp(path, buf, sizeof buf));
    TEST_ASSERT_EQUAL_MEMORY("hello world", buf, 11);
    const uint32_t acks[] = { 1, 1, 2, 3 };
    for (size_t i = 0; i < 4; i++) {
        size_t n = recv_now(a, buf);
        TEST_ASSERT_EQUAL_INT(RDT_OK, rdt_decode(buf, n, &p));
        TEST_ASSERT_EQUAL_UINT32(acks[i], p.seq);
    }
    TEST_ASSERT_EQUAL_INT(RDT_DONE, rdt_receiver_step(b, &r, &out)); /* finished stays so */
    unlink(path);
    close(a);
    close(b);
}

void test_receiver_run_gives_up_when_idle(void)
{
    int a;
    int b;
    struct rdt_receiver r;
    FILE *out = tmpfile();
    udp_pair(&a, &b);
    rdt_receiver_init(&r, rdt_now_ms(), 5, 5);
    TEST_ASSERT_EQUAL_INT(RDT_GAVE_UP, rdt_receiver_run(b, &r, &out));
    TEST_ASSERT_NOT_NULL(out); /* never reached the FIN, so still open */
    fclose(out);
    close(a);
    close(b);
}

void test_receiver_loop_errors(void)
{
    int a;
    int b;
    int fds[2];
    struct sockaddr_in addr;
    struct rdt_receiver r;
    FILE *none = NULL;
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_receiver_step(0, NULL, &none));
    rdt_receiver_init(&r, rdt_now_ms(), 1000, 5);
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_receiver_step(0, &r, NULL));
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_receiver_run(0, NULL, NULL));

    /* Readable, but not a socket. */
    TEST_ASSERT_EQUAL_INT(0, pipe(fds));
    TEST_ASSERT_EQUAL_INT(1, (int)write(fds[1], "x", 1));
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_receiver_step(fds[0], &r, &none));
    close(fds[0]);
    close(fds[1]);

    /* Data to deliver but no file to write it to. */
    udp_pair(&a, &b);
    send_packet(a, RDT_DATA, 0, "x");
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_receiver_step(b, &r, &none));

    /* A file that refuses writes. */
    FILE *ro = fopen("/dev/null", "r");
    TEST_ASSERT_NOT_NULL(ro);
    rdt_receiver_init(&r, rdt_now_ms(), 1000, 5);
    send_packet(a, RDT_DATA, 0, "x");
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_receiver_step(b, &r, &ro));
    fclose(ro);

    /* A file whose close fails: /dev/full takes the buffered write, then fails the flush. */
    FILE *full = fopen("/dev/full", "w");
    TEST_ASSERT_NOT_NULL(full);
    rdt_receiver_init(&r, rdt_now_ms(), 1000, 5);
    send_packet(a, RDT_DATA, 0, "x");
    TEST_ASSERT_EQUAL_INT(RDT_RUNNING, rdt_receiver_step(b, &r, &full));
    send_packet(a, RDT_FIN, 1, NULL);
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_receiver_step(b, &r, &full));
    TEST_ASSERT_NULL(full); /* closed even though the close failed */
    close(a);
    close(b);

    /* The ACK cannot be sent: an unconnected socket has nowhere to send it. */
    int u = udp_bound(&addr);
    int peer = udp_bound(NULL);
    uint8_t packet[RDT_MAX_PACKET];
    size_t n = make_packet(packet, RDT_DATA, 0, "x");
    TEST_ASSERT_EQUAL_INT((int)n, (int)sendto(peer, packet, n, 0, (struct sockaddr *)&addr,
                                              sizeof addr));
    FILE *sink = tmpfile();
    rdt_receiver_init(&r, rdt_now_ms(), 1000, 5);
    TEST_ASSERT_EQUAL_INT(RDT_IO_ERROR, rdt_receiver_step(u, &r, &sink));
    fclose(sink);
    close(u);
    close(peer);
}

void test_whole_transfer_over_real_sockets(void)
{
    /* The real event loops on both ends, taking turns in one thread over two connected
     * loopback sockets. */
    const size_t size = 5 * 1024 + 99;
    uint8_t *data = make_file(size, 15);
    uint8_t *back = malloc(size + 1);
    char path[64];
    int a;
    int b;
    struct rdt_sender s;
    struct rdt_receiver r;
    struct rdt_actions act;
    udp_pair(&a, &b);
    temp_path(path, sizeof path);
    FILE *out = fopen(path, "wb");

    TEST_ASSERT_EQUAL_INT(0, rdt_sender_init(&s, data, size, 2, 1000));
    rdt_receiver_init(&r, rdt_now_ms(), 5000, 20);
    rdt_sender_start(&s, rdt_now_ms(), &act);
    TEST_ASSERT_EQUAL_INT(0, rdt_send_actions(a, &act));
    enum rdt_result sent = RDT_RUNNING;
    enum rdt_result got = RDT_RUNNING;
    for (int i = 0; i < 1000 && (sent == RDT_RUNNING || got == RDT_RUNNING); i++) {
        if (got == RDT_RUNNING) {
            got = rdt_receiver_step(b, &r, &out);
        }
        if (sent == RDT_RUNNING) {
            sent = rdt_sender_step(a, &s);
        }
    }
    TEST_ASSERT_EQUAL_INT(RDT_DONE, sent);
    TEST_ASSERT_EQUAL_INT(RDT_DONE, got);
    TEST_ASSERT_NULL(out);
    TEST_ASSERT_EQUAL_UINT((unsigned)size, (unsigned)slurp(path, back, size + 1));
    TEST_ASSERT_EQUAL_MEMORY(data, back, size);
    rdt_sender_destroy(&s);
    unlink(path);
    close(a);
    close(b);
    free(data);
    free(back);
}

/* ================================================================================ runner */

int main(void)
{
    UNITY_BEGIN();
    /* layer 1 */
    RUN_TEST(test_checksum_rfc1071_example);
    RUN_TEST(test_checksum_odd_length);
    RUN_TEST(test_checksum_of_nothing);
    RUN_TEST(test_checksum_folds_carries);
    RUN_TEST(test_checksum_intact_packet_verifies_to_zero);
    RUN_TEST(test_checksum_catches_every_single_bit_flip);
    RUN_TEST(test_encode_hi_packet_matches_handout);
    RUN_TEST(test_encode_ack3_matches_handout);
    RUN_TEST(test_encode_full_payload);
    RUN_TEST(test_encode_rejects_bad_input);
    RUN_TEST(test_decode_round_trip);
    RUN_TEST(test_decode_ack_and_fin_have_no_payload);
    RUN_TEST(test_decode_too_short);
    RUN_TEST(test_decode_length_does_not_match_size);
    RUN_TEST(test_decode_length_over_1024_even_if_size_matches);
    RUN_TEST(test_decode_unknown_type_or_reserved);
    RUN_TEST(test_decode_bad_checksum);
    RUN_TEST(test_decode_null_arguments);
    /* layer 2: sender */
    RUN_TEST(test_sender_init_rejects_bad_arguments);
    RUN_TEST(test_sender_null_arguments_and_failed_init);
    RUN_TEST(test_sender_cuts_2500_bytes_into_three_packets_and_a_fin);
    RUN_TEST(test_sender_empty_file_is_a_single_fin);
    RUN_TEST(test_sender_exact_multiple_of_1024);
    RUN_TEST(test_sender_window_full_waits_for_an_ack);
    RUN_TEST(test_sender_cumulative_ack_slides_several_packets);
    RUN_TEST(test_sender_duplicate_ack_changes_nothing);
    RUN_TEST(test_sender_ignores_ack_beyond_next_damage_and_non_acks);
    RUN_TEST(test_sender_lost_acks_cost_nothing);
    RUN_TEST(test_sender_timeout_resends_the_whole_window);
    RUN_TEST(test_sender_trace_window_4_six_packets);
    RUN_TEST(test_sender_gives_up_after_10_timeouts);
    RUN_TEST(test_sender_progress_resets_the_give_up_count);
    RUN_TEST(test_sender_window_of_one_is_stop_and_wait);
    /* layer 2: receiver */
    RUN_TEST(test_receiver_delivers_in_order);
    RUN_TEST(test_receiver_reacks_a_duplicate_without_delivering);
    RUN_TEST(test_receiver_discards_a_packet_from_beyond_a_gap);
    RUN_TEST(test_receiver_sends_nothing_for_damaged_packets_or_acks);
    RUN_TEST(test_receiver_fin_linger_and_repeated_fin);
    RUN_TEST(test_receiver_fin_from_beyond_a_gap_is_reacked);
    RUN_TEST(test_receiver_gives_up_after_30_idle_seconds);
    RUN_TEST(test_receiver_trace_with_damage_and_duplicates);
    RUN_TEST(test_receiver_null_arguments);
    /* layer 2: end to end */
    RUN_TEST(test_timeline_from_the_handout);
    RUN_TEST(test_transfer_clean_channel_various_sizes_and_windows);
    RUN_TEST(test_transfer_lossy_20_percent_each_way_several_seeds);
    RUN_TEST(test_transfer_lossy_stop_and_wait);
    /* layer 3 */
    RUN_TEST(test_parse_args_send_with_every_option);
    RUN_TEST(test_parse_args_recv_defaults);
    RUN_TEST(test_parse_args_rejects_wrong_command_lines);
    RUN_TEST(test_parse_args_null_arguments);
    RUN_TEST(test_usage_prints_the_interface);
    RUN_TEST(test_hello_text);
    RUN_TEST(test_now_ms_never_goes_backwards);
    RUN_TEST(test_poll_timeout);
    RUN_TEST(test_open_resolves_names_and_reports_failures);
    RUN_TEST(test_wait_for_a_datagram);
    RUN_TEST(test_wait_treats_port_unreachable_as_silence);
    RUN_TEST(test_wait_reports_a_broken_descriptor);
    RUN_TEST(test_send_actions);
    RUN_TEST(test_register_ok_and_exact_hello_bytes);
    RUN_TEST(test_register_reports_the_err_reason);
    RUN_TEST(test_register_retries_then_gives_up);
    RUN_TEST(test_register_ignores_stray_datagrams);
    RUN_TEST(test_register_with_no_relay_running);
    RUN_TEST(test_register_bad_arguments_and_broken_socket);
    RUN_TEST(test_read_file);
    RUN_TEST(test_sender_run_empty_file);
    RUN_TEST(test_sender_run_gives_up_after_ten_fins);
    RUN_TEST(test_sender_loop_errors);
    RUN_TEST(test_receiver_run_writes_the_file_and_closes_it);
    RUN_TEST(test_receiver_run_gives_up_when_idle);
    RUN_TEST(test_receiver_loop_errors);
    RUN_TEST(test_whole_transfer_over_real_sockets);
    return UNITY_END();
}
