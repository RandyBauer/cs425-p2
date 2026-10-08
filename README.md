# Project 2 - Reliable Data Transfer

- Name: Randy Bauer
- Email: randybauer@u.boisestate.edu
- Class: CS425-001

A reliable file transfer over UDP using Go-Back-N, through the course relay that drops, corrupts
and duplicates datagrams on purpose.

## Building and running

```bash
make all && make check                                    # four builds, then the unit tests
python3 cs425_relay.py --delay 50                         # terminal 1: the relay
./build/release/myapp recv -s jdoe-1 127.0.0.1 out.bin    # terminal 2: receiver first
./build/release/myapp send -s jdoe-1 -w 16 -l 0.1 -c 0.05 127.0.0.1 in.bin   # terminal 3
cmp in.bin out.bin && echo identical
```

Exit codes: **0** for a successful transfer, and for no arguments at all (prints usage). **1**
when the command line is wrong, including an input file that cannot be read or an output file
that cannot be created. **2** when the relay refuses, the relay never answers (five hellos, one
second apart), a socket fails, or the transfer gives up.

## Design

### Three layers, and why they are separate

A relay that damages packets at random cannot be unit tested, but the protocol has to be tested
under exactly that damage. So the protocol logic never touches a socket, a clock or a file.
`src/lab.h` is split into three sections, one per layer, and each layer has its own source file:

| Layer | File | What it does | What it never touches |
| --- | --- | --- | --- |
| 1. Packets | `src/packet.c` | RFC 1071 checksum; encode a packet into the wire format; validate and decode a datagram | any state at all |
| 2. State machines | `src/gbn.c` | The Go-Back-N sender and receiver: a struct each, plus one function per event | `sendto`, `recvfrom`, `poll`, `clock_gettime`, `fwrite` |
| 3. I/O | `src/io.c`, `src/main.c` | Command line, the one UDP socket, the relay hello, the two `poll` loops, `CLOCK_MONOTONIC`, reading and writing the files | protocol decisions |

Each layer uses only the layers above it. The interface of layer 2 is the heart of the design.
Every event function (`rdt_sender_start`, `rdt_sender_on_datagram`, `rdt_sender_on_timeout`,
and the receiver's matching three) takes **the current time in milliseconds as a parameter**. It
fills in a `struct rdt_actions` saying what to do next: which datagrams to send, what payload to
deliver, whether to close the file, when the timer is due, and whether the transfer is still
running, done, or given up. Layer 3's loop is only: wait in `poll` until a datagram arrives or the
deadline passes, hand that event to the state machine, carry out the actions.

That separation is what makes the protocol testable. The same `gbn.c` code runs in three
settings:

- **The real program:** a real socket and the real clock.
- **The unit tests:** a made-up clock and an in-memory channel. A lost packet is just a datagram
  the test does not pass along; a timeout is just a larger number for "now". A 250 ms timer costs
  no time at all.
- **Socket-level tests:** real loopback sockets, still with no relay.

So the test that proves the protocol works, `test_transfer_lossy_20_percent_each_way_several_seeds`,
runs complete transfers through 20% loss, 20% corruption and 20% duplication in each direction,
for eight fixed seeds, and checks every byte. All of that takes milliseconds and gives the same
result on every machine. The handout's own Go-Back-N timeline (window 4, DATA 2 lost) is a unit
test too: `test_timeline_from_the_handout`.

### Protocol decisions

- **Cumulative ACKs, TCP-style:** ACK *n* means every packet below *n* has arrived, so an ACK
  sets `base = n`, and any ACK with *n* <= `base` is a duplicate. This is one off from the
  textbook's Go-Back-N state machine, where ACK *n* means "up to and including *n*" and the sender
  sets `base = n + 1`.
- **One timer.** It starts when a packet is sent with nothing in flight, restarts on every ACK
  that moves `base`, and stops when the FIN is acknowledged. **A duplicate ACK changes nothing,
  not even the timer.** Restarting on duplicates would let a stream of them postpone the
  retransmission forever. An ACK beyond `next` (for a packet never sent) is ignored the same way.
- **Retransmission copies:** `rdt_sender_init` allocates `window` slots once. Packet *seq* is
  encoded into slot `seq % window` when first sent, and a timeout resends `base` to `next - 1`
  byte for byte from those slots.
- **The file is read into memory** by layer 3 (files are at most 16 MiB) and handed to the sender
  as a pointer, so layer 2 cuts packets without ever reading a file.
- **The FIN is packet number `total`** (the count of DATA packets). It is sent only once every
  DATA packet is acknowledged, and it uses the same timer and give-up rule as data.
- **Giving up:** the sender counts timeouts in a row with no ACK that moved `base`. The 10th gives
  up at once (exit 2) without one more resend, so the peer sees the packet 10 times in all.
- **The receiver** writes a payload *before* acknowledging it, because an ACK promises the bytes
  are in the file. It closes the file when the FIN arrives, lingers two seconds answering
  repeated FINs with the same ACK, and gives up after 30 seconds with nothing valid. A damaged
  packet gets no reply at all.

### I/O decisions

- **One UDP socket for the whole run, `connect`ed to the relay,** so the kernel drops datagrams
  from anywhere else. On a connected socket an ICMP "port unreachable" comes back as
  `ECONNREFUSED`. That is treated as silence, which is how the handout says a missing relay
  should look, so with no relay the hello is still tried five times before exit 2.
- **IPv4 only.** The relay listens on 127.0.0.1, and asking `getaddrinfo` for any address family
  lets `localhost` resolve to `::1` first, where nothing is listening.
- **Nothing from the network is trusted before it is checked.** The receive buffer is 2048 bytes,
  bigger than the 1034-byte largest packet, so an oversized datagram cannot be truncated into
  one that looks valid. `rdt_decode` checks the size, the length field, the type and reserved
  bytes, and the checksum, in that order, before reading any payload.
- **Rates are checked with the relay's own rule** (decimal digits, at most one point, 0 to 0.5) and
  sent exactly as typed. So `-l 1e-05` is a command-line error (exit 1) here, not an `ERR` from the
  relay. The hello goes out with no newline and no trailing NUL.
- **The `poll` timeout is never negative.** A deadline that has already passed becomes 0, because
  `poll` reads any negative timeout as "wait forever".
- **The sender checks its timer after every datagram as well,** so steadily arriving duplicates
  cannot keep a timeout from firing.
- **All time is `CLOCK_MONOTONIC`.** In WSL this mattered in practice. The receiver's 30-second
  give-up measured 30.031 s on the monotonic clock and 31.418 s on the wall clock, because WSL's
  wall clock runs fast (see Results).

### Testing

`make check` runs 73 Unity tests, covering every function declared in `src/lab.h`, with 100% line
coverage of `packet.c`, `gbn.c` and `io.c`:

- **Packets:** the RFC 1071 example (`0x220d`), the handout's "Hi!" packet (odd length, `0x9691`)
  and ACK 3 byte for byte, every single-bit flip of a packet caught, and each rejection rule
  (too short, length mismatch, length over 1024, unknown type, reserved byte, bad checksum).
- **Sender:** full window, a cumulative ACK sliding three packets, duplicate ACKs that leave the
  timer alone, lost ACKs that cost nothing (Example 3), a timeout resending the whole window,
  giving up after exactly 10 timeouts, the 2500-byte file of Example 1, an empty file, and a file
  that is an exact multiple of 1024 bytes.
- **Receiver:** in-order delivery, duplicates and out-of-order packets re-ACKed without delivery,
  silence for damaged packets, the FIN, the linger and a repeated FIN (Example 5), and the 30 s
  idle limit.
- **End to end:** the seeded lossy transfer above; clean transfers across sizes 0 to 20000 and
  windows 1 to 64; lossy stop-and-wait (window 1).
- **I/O over loopback, no relay needed:** a fake relay is an unconnected UDP socket whose reply
  is queued *before* the hello is sent (UDP buffers it), so the tests are single-threaded and
  work offline in CI. Failure paths are forced with a pipe (not a socket), an unconnected socket
  (cannot send), `/dev/null` opened read-only (cannot write), `/dev/full` (cannot close), and a
  port nobody listens on (`ECONNREFUSED`).

**Coverage exclusions** (`GCOVR_EXCL`), all of them branches on a library call failing that a test
cannot force: `calloc` in `rdt_sender_init`; `socket`/`connect` in `rdt_open`; `poll` in
`rdt_wait`; a receive failing right after a successful send in `rdt_register`; `fstat`, `malloc`
and `fread` in `rdt_read_file`. `main.c` is excluded by the template's Makefile. It holds only
wiring, and its manual test cases are listed in a comment at its top. All of them were run.

## Results

Setup:
- Measured in WSL (Ubuntu 24.04 on Windows 11).
- Relay started with `python3 cs425_relay.py --delay 50`, with no `--seed`, so every run's
  damage is independent.
- A 1 MiB file from `/dev/urandom`, the default 250 ms timeout, and three runs per row.
- Every copy was checked with `cmp` and was identical, and every run exited 0 on both ends.

| Window | Loss | Runs (s) | Mean time (s) | Throughput (KiB/s) | `time` mean (s) |
| ---: | ---: | --- | ---: | ---: | ---: |
| 1 | 0 | 104.51, 104.57, 103.72 | **104.27** | **9.82** | 108.00 |
| 16 | 0 | 6.58, 6.59, 6.59 | **6.59** | **155.47** | 7.00 |
| 1 | 0.05 | 129.48, 130.73, 128.93 | **129.71** | **7.89** | 134.92 |
| 16 | 0.05 | 25.45, 22.97, 26.54 | **24.99** | **40.98** | 25.44 |

Throughput is 1024 KiB divided by the mean time.

**Why two times.** Each run was timed with bash's `time`, as the handout asks, and also with the
monotonic clock (`/proc/uptime`). The two disagree because WSL's wall clock runs fast. Over a
20-second `sleep`:
- the monotonic clock advanced 20.00 s;
- the wall clock advanced 21.30 s;
- a Windows stopwatch around the whole command, startup included, read 20.75 s.

`time` reads the wall clock, so it overstates every run, by as much as 1.25 s on a 6.6-second
run (window 16, run 1). The table uses the monotonic times. `time`'s means are in the last
column for comparison.

The relay's counters for the lossy runs show what the sender actually transmitted (1025 = 1024
DATA + 1 FIN with no loss):

| Window | Packets sent per run (dropped by the relay) | ACKs sent per run (dropped) |
| ---: | --- | --- |
| 1 | 1124 (46), 1130 (51), 1124 (58) | 1078 (53), 1079 (54), 1066 (41) |
| 16 | 1987 (113), 1869 (103), 2029 (106) | 1874 (108), 1766 (89), 1923 (104) |

### The round-trip time

At window 1 with no loss the sender sent 1025 packets and waited one round trip for each, so
the round trip it saw was 104.27 s / 1025 = **101.7 ms**.

The relay's two 50 ms holds account for 100 ms. To find the rest, I ran the same transfer through
a relay started with `--delay 0`: **0.18 ms** per round trip. So the sender, the receiver, the
relay's Python code and the loopback interface together cost under 0.2 ms. Nearly all of the
extra 1.7 ms is the holds running long, about 0.8 ms each.

The relay sleeps in `select` until the next held datagram is due. CPython rounds that timeout
*up* to a whole millisecond (`selectors.py`: "round away from zero to wait *at least* timeout
seconds"), and the wake-up itself lands a little after that. So each 50 ms hold lasts about
50.8 ms.

### The speedup at window 16

104.27 s / 6.59 s = **15.8 times**: close to 16, but not quite.

With a 101.7 ms round trip, the limit is the window, not the link. Sending a 1034-byte packet
over loopback takes microseconds, so 16 packets leave in a burst. Their ACKs come back one round
trip later, and each one frees room for the next packet.

- **The DATA packets take 64 round trips** (1024 / 16).
- **The FIN takes one more,** because it is sent only after every DATA packet is acknowledged,
  so it always travels alone.

That predicts 65 × 101.7 ms = 6.61 s, against 6.59 s measured, a speedup of 1025 / 65 = 15.8. The
shortfall from 16 is that last, unshared round trip, not any cost of a bigger window. The window
would only stop paying off once 16 packets' transmission time approached the round trip, which on
loopback never happens.

### Why 5% loss hurts window 16 so much more

| | No loss | 5% loss | Slowdown |
| --- | ---: | ---: | ---: |
| Window 1 | 104.3 s | 129.7 s | 1.24 times |
| Window 16 | 6.6 s | 25.0 s | 3.79 times |

**At window 1, every lost packet *or* lost ACK costs exactly one timeout and one resent packet.**

- The sender resent about 101 packets per run: about 10%, from 5% loss in each direction.
- 1025 round trips plus one 250 ms timeout per resend predicts 129.5 s, against 129.7 s measured.
- Window 1 already spends a whole round trip on every packet, and the timeout is only 2.5 round
  trips. So each loss costs about as much time as 2.5 packets: 24% in all.

**At window 16, the same timeout is far more expensive, for three reasons.**

1. **The timeout is long compared with the work it holds up.** With no loss the window delivers a
   packet every 6.4 ms, so a 250 ms timeout costs the time of about 39 packets.
2. **A timeout resends the whole window.** When DATA *k* is lost, the receiver discards *k* + 1
   onward and keeps re-ACKing *k*. The sender cannot slide past *k*, so it sits idle until the
   timer runs out, then resends up to 16 packets, most of which already arrived once. On average
   the sender transmitted 1962 packets per run to deliver 1025: about 937 resends, or 91% extra,
   against 10% at window 1.
3. **Every resent window is exposed to the same 5% loss.** A 16-packet window has a 56% chance of
   losing at least one packet, so one stall often leads straight into the next.

Lost ACKs, by contrast, cost almost nothing at window 16: the next cumulative ACK covers them, as
in the handout's Example 3. At window 1, every lost ACK is a full timeout.

This is the price of Go-Back-N's one-number receiver. A selective-repeat receiver would buffer
*k* + 1 onward, and the sender would resend only *k*.

## Known Bugs or Issues

None known. Limits by design: IPv4 only, like the relay; files up to 16 MiB; windows up to 64.
The receiver creates (or truncates) its output file before registering, so a run that fails
leaves an empty or partial file behind.

## AI Usage

All of `src/lab.h`, `src/packet.c`, `src/gbn.c`, `src/io.c`, `src/main.c` and `tests/lab-test.c`
were written by an AI assistant (Claude) at my request, including the design decisions they
embody: the three-layer split, the event-and-actions interface of the state machines, and the
test strategy. I created the repository from the template, and the assistant ran the builds,
tests, coverage and leak checks and the Task 6 measurements in my WSL environment and on Onyx.
This whole README, including the Experience section, was drafted by the assistant at my request,
from what happened during the project.

## Experience

I did not write the code for this project. I asked an AI assistant (Claude) to write all of it,
run the tests and the measurements, and then explain the finished code to me in a walkthrough. I
chose to get the project working first and learn how it works afterward. I started the project
three days before it was due.

**What the program does, simply.** Sending data over UDP is like mailing postcards. Some cards
get lost. Some get smudged, so a word changes. Some arrive twice. Nobody tells you which. This
program sends a whole file that way and still gets every byte across:

- Every card gets a number, so the receiver can tell a new card from a repeat.
- Every card gets a checksum, a small sum of what is written on it. If the sum does not match,
  the receiver throws the card away, the same as if it were lost.
- The receiver only keeps the next card it is waiting for. It throws away any other card and
  writes back "I have every card before number n."
- If no new confirmation comes back within a quarter of a second, the sender sends again every
  card that has not been confirmed.
- The window lets the sender have several cards in the mail at once. With 16 at a time, the
  1 MiB test file took 6.6 seconds instead of 104.

**What I did.**

- I gave the assistant the assignment page.
- I made the repository from the template. The first time, I picked "Open in a codespace". That
  gives a cloud computer with a copy of the files, but no repository. The second time, I picked
  "Create a new repository", which is the right one. I made the same mix-up on P1.
- I ran the first build. It was clean.
- I turned on the campus VPN so the code could be built and tested on Onyx.

**What happened along the way.**

- The clock in WSL on my computer runs fast. Over a 20-second wait it moved 21.3 seconds. The
  program measures time with the monotonic clock, a clock that only counts forward at a steady
  speed, as the assignment requires. So the program was not affected. Bash's `time` command uses
  the fast clock, so the Results section uses the monotonic times.
- The twelve timed runs for Task 6 take about 15 minutes in all. The first batch stopped after 8
  runs, probably because my computer went to sleep. The last 4 runs were done again the next
  morning.
- Onyx could not be reached until the VPN was on.
