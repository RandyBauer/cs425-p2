/*
 * Layer 2: the Go-Back-N sender and receiver.
 *
 * Nothing in this file calls sendto, recvfrom, poll, clock_gettime or fwrite. The current
 * time arrives as a parameter, and every event function fills in a struct rdt_actions
 * saying what to send, what to deliver and when the timer is due. Layer 3 carries that out
 * with a real socket and clock; the tests carry it out with a made-up clock and an
 * in-memory channel.
 */
#include "lab.h"

#include <stdlib.h>
#include <string.h>

/* Clears an actions list, leaving only the result. */
static void actions_reset(struct rdt_actions *out, enum rdt_result result)
{
    memset(out, 0, sizeof *out);
    out->result = result;
}

/* Appends one datagram to an actions list. Callers never add more than RDT_MAX_WINDOW. */
static void actions_add(struct rdt_actions *out, const uint8_t *bytes, size_t len)
{
    out->dgram[out->ndgram] = bytes;
    out->dgram_len[out->ndgram] = len;
    out->ndgram++;
}

/* ---------------------------------------------------------------------------- sender */

/* Copies the sender's timer and state into an actions list. */
static void sender_report(const struct rdt_sender *s, struct rdt_actions *out)
{
    out->timer_set = s->timer_running;
    out->timer_due_ms = s->timer_due_ms;
    out->result = s->state;
}

/* Encodes packet seq into its retransmission slot and queues it to send. Packet total is
 * the FIN; every other one is DATA cut from the file at seq * 1024. */
static void sender_send_new(struct rdt_sender *s, uint32_t seq, struct rdt_actions *out)
{
    struct rdt_slot *slot = &s->slots[seq % s->window];
    struct rdt_packet p = { .type = RDT_FIN, .seq = seq, .len = 0, .payload = NULL };

    if (seq < s->total) {
        size_t offset = (size_t)seq * RDT_MAX_PAYLOAD;
        size_t left = s->size - offset;
        p.type = RDT_DATA;
        /* At most RDT_MAX_PAYLOAD, so the narrowing is safe. */
        p.len = (uint16_t)(left < RDT_MAX_PAYLOAD ? left : RDT_MAX_PAYLOAD);
        p.payload = s->data + offset;
    }
    slot->len = rdt_encode(&p, slot->bytes, sizeof slot->bytes);
    actions_add(out, slot->bytes, slot->len);
}

/* Sends while the window has room: "while next < base + N and there is still data". Once
 * every DATA packet is acknowledged (base == total), the FIN goes as one more packet. */
static void sender_fill_window(struct rdt_sender *s, struct rdt_actions *out)
{
    while (s->next < s->total && s->next < s->base + s->window) {
        sender_send_new(s, s->next, out);
        s->next++;
    }
    if (s->base == s->total && s->next == s->total) {
        sender_send_new(s, s->next, out);
        s->next++;
    }
}

int rdt_sender_init(struct rdt_sender *s, const uint8_t *data, size_t size, uint32_t window,
                    uint64_t timeout_ms)
{
    if (s == NULL) {
        return -1;
    }
    memset(s, 0, sizeof *s);
    if ((data == NULL && size > 0) || size > RDT_MAX_FILE || window < 1
        || window > RDT_MAX_WINDOW || timeout_ms == 0) {
        return -1;
    }
    s->slots = calloc(window, sizeof *s->slots);
    /* Excluded from coverage: a branch on a library call (calloc) failing, which a test
     * cannot force. */
    if (s->slots == NULL) { // GCOVR_EXCL_START
        return -1;
    } // GCOVR_EXCL_STOP
    s->data = data;
    s->size = size;
    s->window = window;
    s->timeout_ms = timeout_ms;
    /* size is at most 16 MiB, so the packet count fits in 32 bits with room to spare. */
    s->total = (uint32_t)((size + RDT_MAX_PAYLOAD - 1) / RDT_MAX_PAYLOAD);
    s->state = RDT_RUNNING;
    return 0;
}

void rdt_sender_destroy(struct rdt_sender *s)
{
    if (s == NULL) {
        return;
    }
    free(s->slots);
    s->slots = NULL;
}

void rdt_sender_start(struct rdt_sender *s, uint64_t now_ms, struct rdt_actions *out)
{
    if (s == NULL || out == NULL) {
        return;
    }
    actions_reset(out, s->state);
    if (s->state == RDT_RUNNING && s->slots != NULL) {
        sender_fill_window(s, out);
        /* "Start the timer if it was not running." */
        if (s->base < s->next && !s->timer_running) {
            s->timer_running = true;
            s->timer_due_ms = now_ms + s->timeout_ms;
        }
    }
    sender_report(s, out);
}

void rdt_sender_on_datagram(struct rdt_sender *s, const uint8_t *buf, size_t len,
                            uint64_t now_ms, struct rdt_actions *out)
{
    struct rdt_packet p;

    if (s == NULL || out == NULL) {
        return;
    }
    actions_reset(out, s->state);
    /* Only an ACK that moves base counts. seq <= base is a duplicate; seq > next would
     * acknowledge a packet never sent. Both are ignored outright: no state changes at all,
     * and in particular the timer is not restarted, or a stream of duplicate ACKs would
     * postpone the timeout forever. */
    if (s->state == RDT_RUNNING && rdt_decode(buf, len, &p) == RDT_OK && p.type == RDT_ACK
        && p.seq > s->base && p.seq <= s->next) {
        s->base = p.seq; /* cumulative: may slide several packets at once */
        s->fruitless = 0;
        if (s->base == s->total + 1) {
            /* The FIN is acknowledged. */
            s->state = RDT_DONE;
            s->timer_running = false;
        } else {
            sender_fill_window(s, out);
            /* Something is always in flight here: either packets the window allowed, or the
             * FIN, which the fill sends as soon as base reaches total. So the timer
             * restarts rather than stops. */
            s->timer_running = true;
            s->timer_due_ms = now_ms + s->timeout_ms;
        }
    }
    sender_report(s, out);
}

void rdt_sender_on_timeout(struct rdt_sender *s, uint64_t now_ms, struct rdt_actions *out)
{
    if (s == NULL || out == NULL) {
        return;
    }
    actions_reset(out, s->state);
    if (s->state == RDT_RUNNING && s->timer_running && now_ms >= s->timer_due_ms) {
        s->fruitless++;
        if (s->fruitless >= RDT_MAX_TIMEOUTS) {
            /* The 10th timeout in a row with no progress: give up instead of resending. */
            s->state = RDT_GAVE_UP;
            s->timer_running = false;
        } else {
            /* Go back N: resend everything in flight, from the copies, and restart. */
            for (uint32_t seq = s->base; seq < s->next; seq++) {
                const struct rdt_slot *slot = &s->slots[seq % s->window];
                actions_add(out, slot->bytes, slot->len);
            }
            s->timer_due_ms = now_ms + s->timeout_ms;
        }
    }
    sender_report(s, out);
}

/* -------------------------------------------------------------------------- receiver */

/* Copies the receiver's deadline and state into an actions list. */
static void receiver_report(const struct rdt_receiver *r, struct rdt_actions *out)
{
    out->timer_set = r->state == RDT_RUNNING;
    out->timer_due_ms = r->deadline_ms;
    out->result = r->state;
}

/* Queues "ACK expected": every packet below expected has arrived. */
static void receiver_ack(struct rdt_receiver *r, struct rdt_actions *out)
{
    struct rdt_packet ack = { .type = RDT_ACK, .seq = r->expected, .len = 0, .payload = NULL };
    size_t len = rdt_encode(&ack, r->ack, sizeof r->ack);
    actions_add(out, r->ack, len);
}

void rdt_receiver_init(struct rdt_receiver *r, uint64_t now_ms, uint64_t idle_ms,
                       uint64_t linger_ms)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof *r);
    r->idle_ms = idle_ms;
    r->linger_ms = linger_ms;
    r->deadline_ms = now_ms + idle_ms;
    r->state = RDT_RUNNING;
}

void rdt_receiver_on_datagram(struct rdt_receiver *r, const uint8_t *buf, size_t len,
                              uint64_t now_ms, struct rdt_actions *out)
{
    struct rdt_packet p;

    if (r == NULL || out == NULL) {
        return;
    }
    actions_reset(out, r->state);
    /* A packet that fails validation gets no reply at all, exactly like a lost one: there
     * is no NAK. An ACK arriving here is meaningless to a receiver and is ignored too. */
    if (r->state == RDT_RUNNING && rdt_decode(buf, len, &p) == RDT_OK && p.type != RDT_ACK) {
        if (!r->finished) {
            r->deadline_ms = now_ms + r->idle_ms;
            if (p.seq == r->expected && p.type == RDT_DATA) {
                out->deliver = p.payload;
                out->deliver_len = p.len;
                r->expected++;
            } else if (p.seq == r->expected) {
                /* The FIN: close the file, then linger to answer repeats of it. */
                out->fin = true;
                r->finished = true;
                r->expected++;
                r->deadline_ms = now_ms + r->linger_ms;
            }
        }
        /* In order or not, every valid packet is answered with the current cumulative ACK.
         * A duplicate, a packet from beyond a gap, and a repeated FIN during the linger all
         * land here without changing expected. Go-Back-N buffers nothing out of order. */
        receiver_ack(r, out);
    }
    receiver_report(r, out);
}

void rdt_receiver_on_timeout(struct rdt_receiver *r, uint64_t now_ms, struct rdt_actions *out)
{
    if (r == NULL || out == NULL) {
        return;
    }
    actions_reset(out, r->state);
    if (r->state == RDT_RUNNING && now_ms >= r->deadline_ms) {
        r->state = r->finished ? RDT_DONE : RDT_GAVE_UP;
    }
    receiver_report(r, out);
}
