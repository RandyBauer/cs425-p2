# Project 2 - Reliable Data Transfer

- Name: Randy Bauer
- Email: randybauer@u.boisestate.edu
- Class: CS425-001

A file transfer over UDP using Go-Back-N. It runs through the course relay, which drops,
corrupts and duplicates datagrams on purpose.

## Building and running

```bash
make all && make check                                    # four builds, then the unit tests
python3 cs425_relay.py --delay 50                         # terminal 1: the relay
./build/release/myapp recv -s jdoe-1 127.0.0.1 out.bin    # terminal 2: receiver first
./build/release/myapp send -s jdoe-1 -w 16 -l 0.1 -c 0.05 127.0.0.1 in.bin   # terminal 3
cmp in.bin out.bin && echo identical
```

| Exit code | Meaning |
| --- | --- |
| 0 | The transfer completed, or the program was run with no arguments (it prints usage) |
| 1 | The command line is wrong, the input file cannot be read, or the output file cannot be created |
| 2 | The relay refused, the relay never answered (five hellos, one second apart), a socket failed, or the transfer gave up |

## Design

The relay damages packets at random, so a test that runs through it does not give a fixed
result. The protocol logic therefore does not use sockets, clocks or files, and it is tested
without the relay. `src/lab.h` has three sections, one per layer, and each layer has its own
source file:

| Layer | File | What it does | What it does not use |
| --- | --- | --- | --- |
| 1. Packets | `src/packet.c` | RFC 1071 checksum; encodes a packet into the wire format; validates and decodes a datagram | stored state |
| 2. State machines | `src/gbn.c` | The Go-Back-N sender and receiver: a struct each, plus one function per event | `sendto`, `recvfrom`, `poll`, `clock_gettime`, `fwrite` |
| 3. I/O | `src/io.c`, `src/main.c` | The command line, the one UDP socket, the relay hello, the two `poll` loops, `CLOCK_MONOTONIC`, reading and writing the files | protocol decisions |

Each layer uses only the layers above it. Each event function in layer 2 (`rdt_sender_start`,
`rdt_sender_on_datagram`, `rdt_sender_on_timeout`, and the receiver's matching three) takes the
current time in milliseconds as a parameter. It fills in a `struct rdt_actions` with what to do
next: which datagrams to send, what payload to deliver, whether to close the file, when the
timer is due, and whether the transfer is running, done, or given up. Layer 3's loop waits in
`poll` until a datagram arrives or the deadline passes, passes that event to the state machine,
and carries out the actions.

The same `gbn.c` code runs in three settings:

- The real program uses a real socket and the real clock.
- The unit tests use a made-up clock and an in-memory channel. A lost packet is a datagram the
  test does not pass along. A timeout is a larger value for "now", so a 250 ms timer takes no
  real time.
- The socket-level tests use real loopback sockets and no relay.

`test_transfer_lossy_20_percent_each_way_several_seeds` runs complete transfers through 20%
loss, 20% corruption and 20% duplication in each direction, for eight fixed seeds, and checks
every byte. It runs in milliseconds and gives the same result on every machine.
`test_timeline_from_the_handout` reproduces the handout's Go-Back-N timeline (window 4, DATA 2
lost) event by event.

### Protocol decisions

- Cumulative ACKs, as in TCP: ACK *n* means every packet below *n* has arrived. An ACK sets
  `base = n`, and an ACK with *n* <= `base` is a duplicate. The textbook's Go-Back-N state
  machine differs by one: there, ACK *n* means "up to and including *n*" and the sender sets
  `base = n + 1`.
- One timer: it starts when a packet is sent with nothing in flight, restarts on every ACK that
  moves `base`, and stops when the FIN is acknowledged. A duplicate ACK changes no state,
  including the timer. If duplicates restarted the timer, a steady stream of them would postpone
  the retransmission indefinitely. An ACK beyond `next` (for a packet never sent) is also
  ignored.
- Retransmission copies: `rdt_sender_init` allocates `window` slots once. Packet *seq* is encoded
  into slot `seq % window` when it is first sent. A timeout resends `base` to `next - 1` byte for
  byte from those slots.
- File input: layer 3 reads the whole file into memory (files are at most 16 MiB) and passes the
  sender a pointer, so layer 2 cuts packets without reading a file.
- The FIN is packet number `total`, the count of DATA packets. It is sent once every DATA packet
  is acknowledged, and it uses the same timer and give-up rule as data.
- Giving up: the sender counts timeouts in a row with no ACK that moved `base`. On the 10th it
  gives up (exit 2) without resending, so the peer receives the packet at most 10 times.
- The receiver writes a payload before acknowledging it, because the ACK states that the bytes
  are in the file. It closes the file when the FIN arrives, lingers two seconds answering
  repeated FINs with the same ACK, and gives up after 30 seconds with nothing valid. A damaged
  packet gets no reply.

### I/O decisions

- One UDP socket for the whole run, `connect`ed to the relay. The kernel then drops datagrams
  from other addresses. On a connected socket an ICMP "port unreachable" arrives as
  `ECONNREFUSED`. The program treats that as silence, which matches the handout's description of
  a missing relay, so with no relay the hello is still sent five times before exit 2.
- IPv4 only. The relay listens on 127.0.0.1. If `getaddrinfo` is asked for any address family,
  `localhost` can resolve to `::1` first, where nothing is listening.
- Input validation: the receive buffer is 2048 bytes, larger than the 1034-byte maximum packet,
  so an oversized datagram is not truncated into one that looks valid. `rdt_decode` checks the
  size, the length field, the type and reserved bytes, and the checksum, in that order, before
  reading any payload.
- Rates are checked with the relay's own rule (decimal digits, at most one point, 0 to 0.5) and
  sent as typed. `-l 1e-05` is therefore a command-line error (exit 1), not an `ERR` from the
  relay. The hello is sent with no newline and no trailing NUL.
- The `poll` timeout is never negative. A deadline that has already passed becomes 0, because
  `poll` treats a negative timeout as "wait forever".
- The sender also checks its timer after every datagram, so a steady stream of duplicate ACKs
  does not prevent a timeout.
- All time is read from `CLOCK_MONOTONIC`. In WSL the receiver's 30-second give-up measured
  30.031 s on the monotonic clock and 31.418 s on the wall clock (see Results).

### Testing

`make check` runs 73 Unity tests. They cover every function declared in `src/lab.h`, with 100%
line coverage of `packet.c`, `gbn.c` and `io.c`:

- Packets: the RFC 1071 example (`0x220d`), the handout's "Hi!" packet (odd length, `0x9691`)
  and ACK 3 byte for byte, every single-bit flip of a packet, and each rejection rule (too short,
  length mismatch, length over 1024, unknown type, reserved byte, bad checksum).
- Sender: a full window, a cumulative ACK sliding three packets, duplicate ACKs that leave the
  timer alone, lost ACKs that cause no resend (Example 3), a timeout resending the whole window,
  giving up after exactly 10 timeouts, the 2500-byte file of Example 1, an empty file, and a file
  that is an exact multiple of 1024 bytes.
- Receiver: in-order delivery, duplicates and out-of-order packets re-ACKed without delivery, no
  reply to damaged packets, the FIN, the linger and a repeated FIN (Example 5), and the 30 s idle
  limit.
- End to end: the seeded lossy transfer above; clean transfers across sizes 0 to 20000 and
  windows 1 to 64; lossy stop-and-wait (window 1).
- I/O over loopback, with no relay: the fake relay is an unconnected UDP socket whose reply is
  queued before the hello is sent (UDP buffers it), so the tests are single-threaded and run
  offline in CI. Failure paths are forced with a pipe (not a socket), an unconnected socket
  (cannot send), `/dev/null` opened read-only (cannot write), `/dev/full` (cannot close), and a
  port with no listener (`ECONNREFUSED`).

Coverage exclusions (`GCOVR_EXCL`) are all branches on a library call failing that a test cannot
force: `calloc` in `rdt_sender_init`; `socket`/`connect` in `rdt_open`; `poll` in `rdt_wait`; a
receive failing right after a successful send in `rdt_register`; `fstat`, `malloc` and `fread`
in `rdt_read_file`. The template's Makefile excludes `main.c`. It contains only wiring, and its
manual test cases are listed in a comment at its top. All of them were run.

## Results

Setup:

- Measured in WSL (Ubuntu 24.04 on Windows 11).
- The relay was started with `python3 cs425_relay.py --delay 50` and no `--seed`, so each run's
  damage is independent.
- A 1 MiB file from `/dev/urandom`, the default 250 ms timeout, and three runs per row.
- Every copy matched the original under `cmp`, and every run exited 0 on both ends.

| Window | Loss | Runs (s) | Mean time (s) | Throughput (KiB/s) | `time` mean (s) |
| --- | --- | --- | --- | --- | --- |
| 1 | 0 | 104.51, 104.57, 103.72 | 104.27 | 9.82 | 108.00 |
| 16 | 0 | 6.58, 6.59, 6.59 | 6.59 | 155.47 | 7.00 |
| 1 | 0.05 | 129.48, 130.73, 128.93 | 129.71 | 7.89 | 134.92 |
| 16 | 0.05 | 25.45, 22.97, 26.54 | 24.99 | 40.98 | 25.44 |

Throughput is 1024 KiB divided by the mean time.

Timing method: each run was timed with bash's `time`, as the handout asks, and also with the
monotonic clock (`/proc/uptime`). The table's run times, means and throughput use the monotonic
clock. `time` reads the wall clock. In 8 of the 12 runs `time` reported more than the monotonic
clock, by 1.26 to 5.29 s. In the other 4 the two agreed to within 0.01 s. In a separate check of
a 20-second `sleep`, the monotonic clock advanced 20.00 s, the wall clock advanced 21.30 s, and a
Windows stopwatch around the whole command, WSL startup included, read 20.75 s. The last column
gives `time`'s means for comparison.

The relay's counters for the lossy runs show what each side transmitted. With no loss the sender
transmits 1025 packets: 1024 DATA and 1 FIN.

| Window | Packets sent per run (dropped by the relay) | ACKs sent per run (dropped) |
| --- | --- | --- |
| 1 | 1124 (46), 1130 (51), 1124 (58) | 1078 (53), 1079 (54), 1066 (41) |
| 16 | 1987 (113), 1869 (103), 2029 (106) | 1874 (108), 1766 (89), 1923 (104) |

### Round-trip time

At window 1 with no loss, the sender sent 1025 packets and waited one round trip for each. The
round trip it saw was 104.27 s / 1025 = 101.7 ms.

The relay's two 50 ms holds account for 100 ms. The same transfer through a relay started with
`--delay 0` took 0.18 ms per round trip. So the sender, the receiver, the relay's Python code and
the loopback interface together account for about 0.2 ms, and the remaining 1.5 ms comes from the
two holds lasting longer than 50 ms, about 0.8 ms each.

The relay waits in `select` until the next held datagram is due. CPython rounds that timeout up
to a whole millisecond (`selectors.py`: "round away from zero to wait *at least* timeout
seconds"), and the process wakes a little after the timeout ends. Each 50 ms hold therefore
lasts about 50.8 ms.

### Speedup at window 16

104.27 s / 6.59 s = 15.8.

With a 101.7 ms round trip, the window limits the rate, not the link. Sending a 1034-byte packet
over loopback takes microseconds, so 16 packets leave together. Their ACKs return one round trip
later, and each ACK makes room for one more packet.

- The DATA packets take 64 round trips (1024 / 16).
- The FIN takes one more round trip. It is sent only after every DATA packet is acknowledged, so
  it travels alone.

This predicts 65 × 101.7 ms = 6.61 s, against 6.59 s measured, and a speedup of 1025 / 65 =
15.8. The difference from 16 is the FIN's separate round trip. A larger window stops reducing
the time when the time to transmit one window of packets approaches the round-trip time. On
loopback, transmitting a packet takes microseconds.

### Effect of 5% loss

|  | No loss | 5% loss | Slowdown |
| --- | --- | --- | --- |
| Window 1 | 104.3 s | 129.7 s | 1.24 times |
| Window 16 | 6.6 s | 25.0 s | 3.79 times |

At window 1, each lost packet or lost ACK causes one timeout and one resent packet.

- The sender resent about 101 packets per run, about 10%, from 5% loss in each direction.
- 1025 round trips plus one 250 ms timeout per resend predicts 129.5 s. The measured mean is
  129.7 s.
- Window 1 already spends one round trip on every packet, and the timeout is about 2.5 round
  trips. Each loss costs about the time of 2.5 packets, 24% in total.

At window 16, each timeout costs more, for three reasons.

1. The timeout is long compared with the sending rate. With no loss the window delivers a packet
   every 6.4 ms, so a 250 ms timeout costs the time of about 39 packets.
2. A timeout resends the whole window. When DATA *k* is lost, the receiver discards *k* + 1
   onward and keeps re-ACKing *k*. The sender cannot slide past *k*. It waits for the timer, then
   resends up to 16 packets, most of which already arrived once. On average the sender
   transmitted 1962 packets per run to deliver 1025: about 937 resends, 91% extra, against 10%
   at window 1.
3. Each resent window is exposed to the same 5% loss. A 16-packet window has a 56% chance of
   losing at least one packet, so one stall is often followed by another.

Lost ACKs cost little at window 16, because the next cumulative ACK covers them, as in the
handout's Example 3. At window 1, each lost ACK causes a full timeout.

The extra resends come from the Go-Back-N receiver discarding out-of-order packets. A
selective-repeat receiver buffers *k* + 1 onward, and its sender resends only *k*.

## Known Bugs or Issues

No known bugs. Limits: IPv4 only, like the relay; files up to 16 MiB; windows up to 64. The
receiver creates (or truncates) its output file before registering, so a failed run leaves an
empty or partial file.

## AI Usage

All of `src/lab.h`, `src/packet.c`, `src/gbn.c`, `src/io.c`, `src/main.c` and `tests/lab-test.c`
were written by an AI assistant (Claude) at my request. I created the repository from the template.
The assistant ran the builds, tests, coverage and leak checks and the Task 6 measurements in my WSL
environment and on Onyx as per some SKILLS that were previously defined.

## Experience

I did not write the vast majority of the code for this project. I asked an AI assistant (Claude) to
write all of it, run the tests and the measurements. I took a similar approach as the A3 assignment
where my agent worked in stages so that I could more easily follow the code that was being written.
I find the incremental process much better for learning while the concepts are still new to me and
it is easier to audit decisions being made along the way. Since I am using Claude Code specifically,
I also ask for explanations and instruct it to build diagrams to analogize and supplement the diagrams
in the textbook, as well as those in the assignment brief. I also had Claude capture the log outputs for
inspection and then use them to build out the Design section of this README.

**What happened along the way.**

- The clock in WSL on my computer runs fast. Over a 20-second wait it moved 21.3 seconds. The
  program measures time with the monotonic clock, a clock that only counts forward at a steady
  speed, as the assignment requires. So the program was not affected. Bash's `time` command uses
  the fast clock, so the Results section uses the monotonic times.
- The twelve timed runs for Task 6 take about 15 minutes in all. The first batch stopped after 8
  runs, probably because my computer went to sleep. The last 4 runs were done again the next
  morning.
- Onyx could not be reached until the VPN was on but the SKILL for compiling on Onyx did work.
