# Buffer alignment in declick — research note

Status as of 2026-10-14: **measured, not acted on.** A 64-byte aligned
container would buy nothing on macOS and a few percent on x86 Windows, and
only together with a window offset; nothing has been changed. Measured
against `b76351e07`.

## The question

`Channel` keeps every buffer in a `std::vector<double>`. Would a container
that starts on a 64-byte (cache line) boundary make declick faster?

## Where alignment could matter

Almost all the time goes into `g_kernels->filter`, called twice per pass from
`detect()`:

    filter(m_arev, &m_win[from - order], &m_fwd[from], to - from, order + 1)
    filter(m_a,    &m_win[from],         &m_bwd[from], to - from, order + 1)

with `from = pad = maxRun + 3·order`. The coefficients are broadcast one at a
time, so their alignment is irrelevant. That leaves the window reads and the
residual writes. The small vectors (`m_a`, `m_arev`, `m_ra`, `m_solve`,
`m_rhs`, …) are not on the hot path.

`filterAvx2` reads `x + i + k + r + 4j` for r = 0..3 and j = 0..10, with i a
multiple of 32 and k a multiple of 16. Each r touches two phases four doubles
apart, and over r = 0..3 the loads hit all eight 8-byte phases of a cache
line. So **the number of loads that split a cache line is the same at every
offset.** Only the stores, `out + i`, have a fixed phase, and they are 8 per
32 outputs against 44·taps/16 loads.

Whether the hot reads start on a cache line depends on two things:

1. where the allocation starts, and
2. `pad - order = maxRun + 2·order`, in doubles, mod 8.

At the defaults (Max repair 4 ms, order 128), (2) is a multiple of 8 at
22.05, 32, 44.1, 48, 88.2, 96 and 192 kHz. 176.4 kHz is the exception
(maxRun 705, phase 1). Any other Max repair, or an order that is not a
multiple of 4, moves it.

## What the allocators do

- **macOS**: malloc serves allocations of this size (the window is ~21 KB at
  44.1 kHz) from its small zone, which has a 512-byte quantum. Measured: `m_win`
  starts 64-byte aligned. Every Mac figure in the README is therefore already
  the aligned case.
- **Windows x64**: HeapAlloc guarantees 16 bytes, so the window lands 0, 2, 4
  or 6 doubles past a cache line, depending on the heap's state.
- glibc: 16 bytes, same as Windows. Not measured.

## Method

A copy of `declick_core.{h,cpp}` with every `std::vector<double>` replaced by
`std::vector<double, OffsetAlloc<double>>`. The allocator over-allocates,
rounds up to 64 bytes and adds `DECLICK_ALIGN_OFF` doubles, read from the
environment, so one binary can sweep offsets 0–7 with runs alternating. Unset,
it is plain `operator new`. Output is bit-identical at every offset, as it
must be: alignment changes no arithmetic.

    template <class T> struct OffsetAlloc {
        T * allocate(size_t n) {
            char * raw = (char *)malloc(n * sizeof(T) + 128 + 64);
            uintptr_t p = ((uintptr_t)raw + 16 + 63) & ~(uintptr_t)63;
            p += (uintptr_t)declickAlignOff() * 8;
            ((void **)p)[-1] = raw;             // for deallocate()
            return (T *)p;
        }
        ...
    };

Two measurements:

- **End to end**: `declick_cli`, 60 s of Canaro's *Serenata* (1934). Stereo
  on the M1, best of 5. Mono on the Haswell, best of 9, then a 21-round
  head-to-head.
- **Kernel alone**: `filterAvx2` / `filterNeon` in one process. 912 outputs
  (a block plus the default margin), 4000 calls per timing, best of 15 rounds,
  input and output both at the given offset. Run in both offset orders on the
  Haswell, to rule out drift.

Machines: a 2014 MacBook Pro (Haswell i5-4258U, load average 4–6 throughout)
and an M1 Mac mini (quiet).

## Results

### M1 — no effect

End to end, time relative to the fastest offset:

| order | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | 1.000 | 1.003 | 1.000 | 1.005 | 1.002 | 1.004 | 1.001 | 1.004 |
| 256 | 1.013 | 1.016 | 1.000 | 1.016 | 1.012 | 1.016 | 1.013 | 1.016 |

The medians at order 256 agree to 0.3%. Even offsets (16-byte aligned, a whole
NEON register) are about 0.3% ahead of odd ones. The kernel alone is flat to
0.7%, with offset 0 slowest.

### Haswell — 2–5% in the kernel

`filterAvx2` alone, relative to offset 0. Two runs with ascending offsets and
two descending all agree; this is one ascending run:

| taps | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 129 | 1.000 | 1.011 | 1.046 | 1.071 | 1.068 | 1.057 | 1.043 | 1.053 |
| 257 | 1.000 | 1.013 | 1.052 | 1.038 | 1.035 | 1.053 | 1.060 | 1.055 |

The best case is 9.5 ns per output at 129 taps and 19.2 ns at 257. The
load-split argument above predicts that offsets 5–7 would pay for split stores
and 1–4 would not. Instead every non-zero offset is slower, which this note
does not explain.

End to end at order 128, offset 0 against offset 3, in alternating order
over 21 rounds:

| offset | best | median |
| --- | --- | --- |
| 0 | 0.677 s | 0.753 s |
| 3 | 0.723 s | 0.781 s |

That is 6.8% by the best run and 3.7% by the median. The full sweep over all
eight offsets was too noisy at this load to rank them: the fastest offset
differed between orders and between best and median.

## Conclusion

- On **macOS** an aligned container changes nothing. The buffers are already
  aligned, and the M1 would not care if they weren't.
- On **x86 Windows** it is probably worth a few percent on Intel: the 16-byte
  heap leaves the window 0–6 doubles off a line. Across the four kernel runs,
  offsets 2, 4 and 6 cost about 2–7%, with one outlier at 12.7%. Not
  measured on Windows or on the Zen 4.
- **Aligning the allocation alone is not enough**, because of
  `maxRun + 2·order` above. A fix that holds at every setting needs both:
  - a 64-byte allocator on the window and residual buffers, e.g.
    `std::vector<double, Aligned64<double>>` over C++17
    `operator new(size, std::align_val_t(64))`, so `heapBytes()` and the
    no-allocation-on-reconfigure rule keep working; and
  - a lead of `(8 - (pad - order) % 8) % 8` doubles in front of the window, so
    the forward filter's input starts on a line. The backward filter reads
    `order` doubles later, so both line up only when order is a multiple of 8;
    the forward and backward outputs share a phase with each other.

If this is picked up: implement both parts, check the outputs are identical,
and measure where the gain is expected — Windows x64 on Intel, and the Zen 4.
On the Haswell, check the change does not regress; it cannot show a gain there
at the defaults, because they are already aligned on macOS.
