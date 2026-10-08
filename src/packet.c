/*
 * Layer 1: packets. Checksum, encode, decode. Pure functions with no state and no I/O.
 */
#include "lab.h"

#include <arpa/inet.h>
#include <string.h>

/* Byte offsets of the header fields. */
#define OFF_TYPE 0u
#define OFF_RESERVED 1u
#define OFF_CHECKSUM 2u
#define OFF_SEQ 4u
#define OFF_LENGTH 8u

uint16_t rdt_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;

    if (data == NULL) {
        len = 0;
    }
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)data[i] << 8) | (uint32_t)data[i + 1];
        /* Fold the carry back in every step, so sum never exceeds 0xffff between words. */
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    if (len % 2 != 0) {
        /* An odd last byte is the high half of a word padded with zero. The pad is only
         * arithmetic: nothing past the end of data is read. */
        sum += (uint32_t)data[len - 1] << 8;
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

size_t rdt_encode(const struct rdt_packet *p, uint8_t *buf, size_t cap)
{
    if (p == NULL || buf == NULL || p->type > RDT_FIN || p->len > RDT_MAX_PAYLOAD
        || (p->type != RDT_DATA && p->len != 0) || (p->len > 0 && p->payload == NULL)) {
        return 0;
    }
    size_t total = RDT_HEADER_LEN + (size_t)p->len;
    if (cap < total) {
        return 0;
    }

    uint32_t seq = htonl(p->seq);
    uint16_t length = htons(p->len);
    buf[OFF_TYPE] = p->type;
    buf[OFF_RESERVED] = 0;
    buf[OFF_CHECKSUM] = 0; /* the checksum is computed with its own field zero */
    buf[OFF_CHECKSUM + 1] = 0;
    memcpy(buf + OFF_SEQ, &seq, sizeof seq);
    memcpy(buf + OFF_LENGTH, &length, sizeof length);
    if (p->len > 0) {
        memcpy(buf + RDT_HEADER_LEN, p->payload, p->len);
    }
    uint16_t sum = htons(rdt_checksum(buf, total));
    memcpy(buf + OFF_CHECKSUM, &sum, sizeof sum);
    return total;
}

enum rdt_status rdt_decode(const uint8_t *buf, size_t len, struct rdt_packet *out)
{
    uint16_t length;
    uint32_t seq;

    if (buf == NULL || out == NULL) {
        return RDT_ERR_ARG;
    }
    if (len < RDT_HEADER_LEN) {
        return RDT_ERR_SHORT;
    }
    memcpy(&length, buf + OFF_LENGTH, sizeof length);
    length = ntohs(length);
    /* len is what the receive call returned; the length field is only the sender's claim,
     * and the relay flips bits in it. Check it before trusting it for anything. */
    if (length > RDT_MAX_PAYLOAD || RDT_HEADER_LEN + (size_t)length != len) {
        return RDT_ERR_LENGTH;
    }
    if (buf[OFF_TYPE] > RDT_FIN || buf[OFF_RESERVED] != 0) {
        return RDT_ERR_TYPE;
    }
    /* Summing an intact packet with its checksum in gives 0xffff, whose complement is 0. */
    if (rdt_checksum(buf, len) != 0) {
        return RDT_ERR_CHECKSUM;
    }

    memcpy(&seq, buf + OFF_SEQ, sizeof seq);
    out->type = buf[OFF_TYPE];
    out->seq = ntohl(seq);
    out->len = length;
    out->payload = length > 0 ? buf + RDT_HEADER_LEN : NULL;
    return RDT_OK;
}
