# Submission Report

- Submission generated at 10/08/2026 at 23:19:59

- Machine info: Linux runnervmmprz5 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux

## Note to Students

Please read this report carefully before submission.
Ensure that all sections are complete and accurate.
Look for any errors in the build or test outputs.
If you find any issues, correct them before submitting.
Post any questions on the class discussion board for help.


---

## README

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
write most of it, run the tests and the measurements. I took a similar approach as the A3 assignment
where my agent worked in stages so that I could more easily follow the code that was being written.
I find the incremental process much better for learning while the concepts are still new to me and
it is easier to audit decisions being made along the way. Since I am using Claude Code specifically,
I also ask for explanations and instruct it to build diagrams to analogize and supplement the diagrams
in the textbook, as well as those in the assignment brief. I also had Claude capture the log outputs for
inspection and then use them to build out the Results section of this README.

**What happened along the way.**

- The clock in WSL on my computer runs fast. Over a 20-second wait it moved 21.3 seconds. The
  program measures time with the monotonic clock, a clock that only counts forward at a steady
  speed, as the assignment requires. So the program was not affected. Bash's `time` command uses
  the fast clock, so the Results section uses the monotonic times.
- The twelve timed runs for Task 6 take about 15 minutes in all. The first batch stopped after 8
  runs, probably because my computer went to sleep. The last 4 runs were done again the next
  morning.
- Onyx could not be reached until the VPN was on but the SKILL for compiling on Onyx did work.

---


## Build Output

This section was generated by running `make all` in the project root directory.

```bash
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/io.c -o build/debug/io.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/packet.c -o build/debug/packet.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/gbn.c -o build/debug/gbn.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug/main.c.o
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address build/debug/io.c.o build/debug/packet.c.o build/debug/gbn.c.o build/debug/main.c.o -o build/debug/myapp_d -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/io.c -o build/release/io.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/packet.c -o build/release/packet.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/gbn.c -o build/release/gbn.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/main.c -o build/release/main.c.o
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion build/release/io.c.o build/release/packet.c.o build/release/gbn.c.o build/release/main.c.o -o build/release/myapp 
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/io.c -o build/tests/io.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/packet.c -o build/tests/packet.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/gbn.c -o build/tests/gbn.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/main.c -o build/tests/main.c.o
mkdir -p build/tests/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/lab-test.c -o build/tests/lab-test.c.o
mkdir -p build/tests/harness/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/harness/unity.c -o build/tests/harness/unity.c.o
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage build/tests/io.c.o build/tests/packet.c.o build/tests/gbn.c.o build/tests/main.c.o build/tests/lab-test.c.o build/tests/harness/unity.c.o -o build/tests/myapp_t -fprofile-arcs -ftest-coverage
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/io.c -o build/debug-test/io.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/packet.c -o build/debug-test/packet.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/gbn.c -o build/debug-test/gbn.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug-test/main.c.o
mkdir -p build/debug-test/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/lab-test.c -o build/debug-test/lab-test.c.o
mkdir -p build/debug-test/harness/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/harness/unity.c -o build/debug-test/harness/unity.c.o
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address build/debug-test/io.c.o build/debug-test/packet.c.o build/debug-test/gbn.c.o build/debug-test/main.c.o build/debug-test/lab-test.c.o build/debug-test/harness/unity.c.o -o build/debug-test/myapp_td -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
Builds completed. You can run the application with: ./build/release/myapp
You can run the debug build with: ./build/debug/myapp_d
You can run the test build with: ./build/tests/myapp_t
You can run the debug-test build with: ./build/debug-test/myapp_td
```

---

## Coverage Report

This section was generated by running `make report` in the project root directory.

```bash
tests/lab-test.c:1757:test_checksum_rfc1071_example:PASS
tests/lab-test.c:1758:test_checksum_odd_length:PASS
tests/lab-test.c:1759:test_checksum_of_nothing:PASS
tests/lab-test.c:1760:test_checksum_folds_carries:PASS
tests/lab-test.c:1761:test_checksum_intact_packet_verifies_to_zero:PASS
tests/lab-test.c:1762:test_checksum_catches_every_single_bit_flip:PASS
tests/lab-test.c:1763:test_encode_hi_packet_matches_handout:PASS
tests/lab-test.c:1764:test_encode_ack3_matches_handout:PASS
tests/lab-test.c:1765:test_encode_full_payload:PASS
tests/lab-test.c:1766:test_encode_rejects_bad_input:PASS
tests/lab-test.c:1767:test_decode_round_trip:PASS
tests/lab-test.c:1768:test_decode_ack_and_fin_have_no_payload:PASS
tests/lab-test.c:1769:test_decode_too_short:PASS
tests/lab-test.c:1770:test_decode_length_does_not_match_size:PASS
tests/lab-test.c:1771:test_decode_length_over_1024_even_if_size_matches:PASS
tests/lab-test.c:1772:test_decode_unknown_type_or_reserved:PASS
tests/lab-test.c:1773:test_decode_bad_checksum:PASS
tests/lab-test.c:1774:test_decode_null_arguments:PASS
tests/lab-test.c:1776:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:1777:test_sender_null_arguments_and_failed_init:PASS
tests/lab-test.c:1778:test_sender_cuts_2500_bytes_into_three_packets_and_a_fin:PASS
tests/lab-test.c:1779:test_sender_empty_file_is_a_single_fin:PASS
tests/lab-test.c:1780:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:1781:test_sender_window_full_waits_for_an_ack:PASS
tests/lab-test.c:1782:test_sender_cumulative_ack_slides_several_packets:PASS
tests/lab-test.c:1783:test_sender_duplicate_ack_changes_nothing:PASS
tests/lab-test.c:1784:test_sender_ignores_ack_beyond_next_damage_and_non_acks:PASS
tests/lab-test.c:1785:test_sender_lost_acks_cost_nothing:PASS
tests/lab-test.c:1786:test_sender_timeout_resends_the_whole_window:PASS
tests/lab-test.c:1787:test_sender_trace_window_4_six_packets:PASS
tests/lab-test.c:1788:test_sender_gives_up_after_10_timeouts:PASS
tests/lab-test.c:1789:test_sender_progress_resets_the_give_up_count:PASS
tests/lab-test.c:1790:test_sender_window_of_one_is_stop_and_wait:PASS
tests/lab-test.c:1792:test_receiver_delivers_in_order:PASS
tests/lab-test.c:1793:test_receiver_reacks_a_duplicate_without_delivering:PASS
tests/lab-test.c:1794:test_receiver_discards_a_packet_from_beyond_a_gap:PASS
tests/lab-test.c:1795:test_receiver_sends_nothing_for_damaged_packets_or_acks:PASS
tests/lab-test.c:1796:test_receiver_fin_linger_and_repeated_fin:PASS
tests/lab-test.c:1797:test_receiver_fin_from_beyond_a_gap_is_reacked:PASS
tests/lab-test.c:1798:test_receiver_gives_up_after_30_idle_seconds:PASS
tests/lab-test.c:1799:test_receiver_trace_with_damage_and_duplicates:PASS
tests/lab-test.c:1800:test_receiver_null_arguments:PASS
tests/lab-test.c:1802:test_timeline_from_the_handout:PASS
tests/lab-test.c:1803:test_transfer_clean_channel_various_sizes_and_windows:PASS
tests/lab-test.c:1804:test_transfer_lossy_20_percent_each_way_several_seeds:PASS
tests/lab-test.c:1805:test_transfer_lossy_stop_and_wait:PASS
tests/lab-test.c:1807:test_parse_args_send_with_every_option:PASS
tests/lab-test.c:1808:test_parse_args_recv_defaults:PASS
tests/lab-test.c:1809:test_parse_args_rejects_wrong_command_lines:PASS
tests/lab-test.c:1810:test_parse_args_null_arguments:PASS
tests/lab-test.c:1811:test_usage_prints_the_interface:PASS
tests/lab-test.c:1812:test_hello_text:PASS
tests/lab-test.c:1813:test_now_ms_never_goes_backwards:PASS
tests/lab-test.c:1814:test_poll_timeout:PASS
tests/lab-test.c:1815:test_open_resolves_names_and_reports_failures:PASS
tests/lab-test.c:1816:test_wait_for_a_datagram:PASS
tests/lab-test.c:1817:test_wait_treats_port_unreachable_as_silence:PASS
tests/lab-test.c:1818:test_wait_reports_a_broken_descriptor:PASS
tests/lab-test.c:1819:test_send_actions:PASS
tests/lab-test.c:1820:test_register_ok_and_exact_hello_bytes:PASS
tests/lab-test.c:1821:test_register_reports_the_err_reason:PASS
tests/lab-test.c:1822:test_register_retries_then_gives_up:PASS
tests/lab-test.c:1823:test_register_ignores_stray_datagrams:PASS
tests/lab-test.c:1824:test_register_with_no_relay_running:PASS
tests/lab-test.c:1825:test_register_bad_arguments_and_broken_socket:PASS
tests/lab-test.c:1826:test_read_file:PASS
tests/lab-test.c:1827:test_sender_run_empty_file:PASS
tests/lab-test.c:1828:test_sender_run_gives_up_after_ten_fins:PASS
tests/lab-test.c:1829:test_sender_loop_errors:PASS
tests/lab-test.c:1830:test_receiver_run_writes_the_file_and_closes_it:PASS
tests/lab-test.c:1831:test_receiver_run_gives_up_when_idle:PASS
tests/lab-test.c:1832:test_receiver_loop_errors:PASS
tests/lab-test.c:1833:test_whole_transfer_over_real_sockets:PASS

-----------------------
73 Tests 0 Failures 0 Ignored 
OK
./build/tests/myapp_t
tests/lab-test.c:1757:test_checksum_rfc1071_example:PASS
tests/lab-test.c:1758:test_checksum_odd_length:PASS
tests/lab-test.c:1759:test_checksum_of_nothing:PASS
tests/lab-test.c:1760:test_checksum_folds_carries:PASS
tests/lab-test.c:1761:test_checksum_intact_packet_verifies_to_zero:PASS
tests/lab-test.c:1762:test_checksum_catches_every_single_bit_flip:PASS
tests/lab-test.c:1763:test_encode_hi_packet_matches_handout:PASS
tests/lab-test.c:1764:test_encode_ack3_matches_handout:PASS
tests/lab-test.c:1765:test_encode_full_payload:PASS
tests/lab-test.c:1766:test_encode_rejects_bad_input:PASS
tests/lab-test.c:1767:test_decode_round_trip:PASS
tests/lab-test.c:1768:test_decode_ack_and_fin_have_no_payload:PASS
tests/lab-test.c:1769:test_decode_too_short:PASS
tests/lab-test.c:1770:test_decode_length_does_not_match_size:PASS
tests/lab-test.c:1771:test_decode_length_over_1024_even_if_size_matches:PASS
tests/lab-test.c:1772:test_decode_unknown_type_or_reserved:PASS
tests/lab-test.c:1773:test_decode_bad_checksum:PASS
tests/lab-test.c:1774:test_decode_null_arguments:PASS
tests/lab-test.c:1776:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:1777:test_sender_null_arguments_and_failed_init:PASS
tests/lab-test.c:1778:test_sender_cuts_2500_bytes_into_three_packets_and_a_fin:PASS
tests/lab-test.c:1779:test_sender_empty_file_is_a_single_fin:PASS
tests/lab-test.c:1780:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:1781:test_sender_window_full_waits_for_an_ack:PASS
tests/lab-test.c:1782:test_sender_cumulative_ack_slides_several_packets:PASS
tests/lab-test.c:1783:test_sender_duplicate_ack_changes_nothing:PASS
tests/lab-test.c:1784:test_sender_ignores_ack_beyond_next_damage_and_non_acks:PASS
tests/lab-test.c:1785:test_sender_lost_acks_cost_nothing:PASS
tests/lab-test.c:1786:test_sender_timeout_resends_the_whole_window:PASS
tests/lab-test.c:1787:test_sender_trace_window_4_six_packets:PASS
tests/lab-test.c:1788:test_sender_gives_up_after_10_timeouts:PASS
tests/lab-test.c:1789:test_sender_progress_resets_the_give_up_count:PASS
tests/lab-test.c:1790:test_sender_window_of_one_is_stop_and_wait:PASS
tests/lab-test.c:1792:test_receiver_delivers_in_order:PASS
tests/lab-test.c:1793:test_receiver_reacks_a_duplicate_without_delivering:PASS
tests/lab-test.c:1794:test_receiver_discards_a_packet_from_beyond_a_gap:PASS
tests/lab-test.c:1795:test_receiver_sends_nothing_for_damaged_packets_or_acks:PASS
tests/lab-test.c:1796:test_receiver_fin_linger_and_repeated_fin:PASS
tests/lab-test.c:1797:test_receiver_fin_from_beyond_a_gap_is_reacked:PASS
tests/lab-test.c:1798:test_receiver_gives_up_after_30_idle_seconds:PASS
tests/lab-test.c:1799:test_receiver_trace_with_damage_and_duplicates:PASS
tests/lab-test.c:1800:test_receiver_null_arguments:PASS
tests/lab-test.c:1802:test_timeline_from_the_handout:PASS
tests/lab-test.c:1803:test_transfer_clean_channel_various_sizes_and_windows:PASS
tests/lab-test.c:1804:test_transfer_lossy_20_percent_each_way_several_seeds:PASS
tests/lab-test.c:1805:test_transfer_lossy_stop_and_wait:PASS
tests/lab-test.c:1807:test_parse_args_send_with_every_option:PASS
tests/lab-test.c:1808:test_parse_args_recv_defaults:PASS
tests/lab-test.c:1809:test_parse_args_rejects_wrong_command_lines:PASS
tests/lab-test.c:1810:test_parse_args_null_arguments:PASS
tests/lab-test.c:1811:test_usage_prints_the_interface:PASS
tests/lab-test.c:1812:test_hello_text:PASS
tests/lab-test.c:1813:test_now_ms_never_goes_backwards:PASS
tests/lab-test.c:1814:test_poll_timeout:PASS
tests/lab-test.c:1815:test_open_resolves_names_and_reports_failures:PASS
tests/lab-test.c:1816:test_wait_for_a_datagram:PASS
tests/lab-test.c:1817:test_wait_treats_port_unreachable_as_silence:PASS
tests/lab-test.c:1818:test_wait_reports_a_broken_descriptor:PASS
tests/lab-test.c:1819:test_send_actions:PASS
tests/lab-test.c:1820:test_register_ok_and_exact_hello_bytes:PASS
tests/lab-test.c:1821:test_register_reports_the_err_reason:PASS
tests/lab-test.c:1822:test_register_retries_then_gives_up:PASS
tests/lab-test.c:1823:test_register_ignores_stray_datagrams:PASS
tests/lab-test.c:1824:test_register_with_no_relay_running:PASS
tests/lab-test.c:1825:test_register_bad_arguments_and_broken_socket:PASS
tests/lab-test.c:1826:test_read_file:PASS
tests/lab-test.c:1827:test_sender_run_empty_file:PASS
tests/lab-test.c:1828:test_sender_run_gives_up_after_ten_fins:PASS
tests/lab-test.c:1829:test_sender_loop_errors:PASS
tests/lab-test.c:1830:test_receiver_run_writes_the_file_and_closes_it:PASS
tests/lab-test.c:1831:test_receiver_run_gives_up_when_idle:PASS
tests/lab-test.c:1832:test_receiver_loop_errors:PASS
tests/lab-test.c:1833:test_whole_transfer_over_real_sockets:PASS

-----------------------
73 Tests 0 Failures 0 Ignored 
OK
mkdir -p ./build/report/html
mkdir -p ./build/report/txt
gcovr -r . --html --html-details --exclude-directories build/tests/harness --exclude '.*main\.c$' --exclude '.*test\.c$' -o ./build/report/html/coverage_report.html
(INFO) Reading coverage data...

(INFO) Writing coverage report...

gcovr -r . --txt                 --exclude-directories build/tests/harness --exclude '.*main\.c$' --exclude '.*test\.c$'
(INFO) Reading coverage data...

(INFO) Writing coverage report...

------------------------------------------------------------------------------
                           GCC Code Coverage Report
Directory: .
------------------------------------------------------------------------------
File                                       Lines     Exec  Cover   Missing
------------------------------------------------------------------------------
src/gbn.c                                    136      136   100%
src/io.c                                     272      272   100%
src/packet.c                                  50       50   100%
------------------------------------------------------------------------------
TOTAL                                        458      458   100%
------------------------------------------------------------------------------
```

---

## Address Sanitizer Report

This section was generated by running `make leak-test` in the project root directory.

```bash
tests/lab-test.c:1757:test_checksum_rfc1071_example:PASS
tests/lab-test.c:1758:test_checksum_odd_length:PASS
tests/lab-test.c:1759:test_checksum_of_nothing:PASS
tests/lab-test.c:1760:test_checksum_folds_carries:PASS
tests/lab-test.c:1761:test_checksum_intact_packet_verifies_to_zero:PASS
tests/lab-test.c:1762:test_checksum_catches_every_single_bit_flip:PASS
tests/lab-test.c:1763:test_encode_hi_packet_matches_handout:PASS
tests/lab-test.c:1764:test_encode_ack3_matches_handout:PASS
tests/lab-test.c:1765:test_encode_full_payload:PASS
tests/lab-test.c:1766:test_encode_rejects_bad_input:PASS
tests/lab-test.c:1767:test_decode_round_trip:PASS
tests/lab-test.c:1768:test_decode_ack_and_fin_have_no_payload:PASS
tests/lab-test.c:1769:test_decode_too_short:PASS
tests/lab-test.c:1770:test_decode_length_does_not_match_size:PASS
tests/lab-test.c:1771:test_decode_length_over_1024_even_if_size_matches:PASS
tests/lab-test.c:1772:test_decode_unknown_type_or_reserved:PASS
tests/lab-test.c:1773:test_decode_bad_checksum:PASS
tests/lab-test.c:1774:test_decode_null_arguments:PASS
tests/lab-test.c:1776:test_sender_init_rejects_bad_arguments:PASS
tests/lab-test.c:1777:test_sender_null_arguments_and_failed_init:PASS
tests/lab-test.c:1778:test_sender_cuts_2500_bytes_into_three_packets_and_a_fin:PASS
tests/lab-test.c:1779:test_sender_empty_file_is_a_single_fin:PASS
tests/lab-test.c:1780:test_sender_exact_multiple_of_1024:PASS
tests/lab-test.c:1781:test_sender_window_full_waits_for_an_ack:PASS
tests/lab-test.c:1782:test_sender_cumulative_ack_slides_several_packets:PASS
tests/lab-test.c:1783:test_sender_duplicate_ack_changes_nothing:PASS
tests/lab-test.c:1784:test_sender_ignores_ack_beyond_next_damage_and_non_acks:PASS
tests/lab-test.c:1785:test_sender_lost_acks_cost_nothing:PASS
tests/lab-test.c:1786:test_sender_timeout_resends_the_whole_window:PASS
tests/lab-test.c:1787:test_sender_trace_window_4_six_packets:PASS
tests/lab-test.c:1788:test_sender_gives_up_after_10_timeouts:PASS
tests/lab-test.c:1789:test_sender_progress_resets_the_give_up_count:PASS
tests/lab-test.c:1790:test_sender_window_of_one_is_stop_and_wait:PASS
tests/lab-test.c:1792:test_receiver_delivers_in_order:PASS
tests/lab-test.c:1793:test_receiver_reacks_a_duplicate_without_delivering:PASS
tests/lab-test.c:1794:test_receiver_discards_a_packet_from_beyond_a_gap:PASS
tests/lab-test.c:1795:test_receiver_sends_nothing_for_damaged_packets_or_acks:PASS
tests/lab-test.c:1796:test_receiver_fin_linger_and_repeated_fin:PASS
tests/lab-test.c:1797:test_receiver_fin_from_beyond_a_gap_is_reacked:PASS
tests/lab-test.c:1798:test_receiver_gives_up_after_30_idle_seconds:PASS
tests/lab-test.c:1799:test_receiver_trace_with_damage_and_duplicates:PASS
tests/lab-test.c:1800:test_receiver_null_arguments:PASS
tests/lab-test.c:1802:test_timeline_from_the_handout:PASS
tests/lab-test.c:1803:test_transfer_clean_channel_various_sizes_and_windows:PASS
tests/lab-test.c:1804:test_transfer_lossy_20_percent_each_way_several_seeds:PASS
tests/lab-test.c:1805:test_transfer_lossy_stop_and_wait:PASS
tests/lab-test.c:1807:test_parse_args_send_with_every_option:PASS
tests/lab-test.c:1808:test_parse_args_recv_defaults:PASS
tests/lab-test.c:1809:test_parse_args_rejects_wrong_command_lines:PASS
tests/lab-test.c:1810:test_parse_args_null_arguments:PASS
tests/lab-test.c:1811:test_usage_prints_the_interface:PASS
tests/lab-test.c:1812:test_hello_text:PASS
tests/lab-test.c:1813:test_now_ms_never_goes_backwards:PASS
tests/lab-test.c:1814:test_poll_timeout:PASS
tests/lab-test.c:1815:test_open_resolves_names_and_reports_failures:PASS
tests/lab-test.c:1816:test_wait_for_a_datagram:PASS
tests/lab-test.c:1817:test_wait_treats_port_unreachable_as_silence:PASS
tests/lab-test.c:1818:test_wait_reports_a_broken_descriptor:PASS
tests/lab-test.c:1819:test_send_actions:PASS
tests/lab-test.c:1820:test_register_ok_and_exact_hello_bytes:PASS
tests/lab-test.c:1821:test_register_reports_the_err_reason:PASS
tests/lab-test.c:1822:test_register_retries_then_gives_up:PASS
tests/lab-test.c:1823:test_register_ignores_stray_datagrams:PASS
tests/lab-test.c:1824:test_register_with_no_relay_running:PASS
tests/lab-test.c:1825:test_register_bad_arguments_and_broken_socket:PASS
tests/lab-test.c:1826:test_read_file:PASS
tests/lab-test.c:1827:test_sender_run_empty_file:PASS
tests/lab-test.c:1828:test_sender_run_gives_up_after_ten_fins:PASS
tests/lab-test.c:1829:test_sender_loop_errors:PASS
tests/lab-test.c:1830:test_receiver_run_writes_the_file_and_closes_it:PASS
tests/lab-test.c:1831:test_receiver_run_gives_up_when_idle:PASS
tests/lab-test.c:1832:test_receiver_loop_errors:PASS
tests/lab-test.c:1833:test_whole_transfer_over_real_sockets:PASS

-----------------------
73 Tests 0 Failures 0 Ignored 
OK
```

---

## Src Files
### gbn.c

```c

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

```

### io.c

```c

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

```

### lab.h

```c

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

```

### main.c

```c

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

```

### packet.c

```c

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

```

## Tests Files
### lab-test.c

```c

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

```

## Scripts Files
Report generated on 10/08/2026 at 23:20:02


---

## End of Report

SHA-256 Hash of the report: fc7954343db031628f08ea55e1efc6b27e122d084458af07db42884c3f8a417e

Do not edit the generated report. Any changes will be reported as academic dishonesty

---
## GitHub Info
- GitHub repo name: RandyBauer/cs425-p2
- The repository visibility is public.
- The workflow was triggered by RandyBauer
