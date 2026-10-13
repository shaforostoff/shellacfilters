# TODO

## declick: measure tap blocking on Haswell

`filterAvx2()` in `plugins/foobar2000_dsp/foo_dsp_declick/declick_core.cpp`
now takes the taps in groups of four a stride of four apart (k, k+4, k+8,
k+12), so each unaligned load of `x` feeds up to four FMAs: 11 loads and 4
broadcasts per 32 multiply-adds instead of 36 loads. x86 runs groups of two.

- Zen 4 (dev box, three load ports): only **+2-3%** end to end, at orders 64,
  128 and 256.
- Haswell (2014 MacBook Air/Pro, two load ports, a cache-line-split load costs
  two): **not measured**. By load-port arithmetic, tap-at-a-time sits at about
  67% FMA utilisation and blocked should be FMA-bound, so a gain is plausible
  on the filter-heavy part of the run time (fit, detection residuals).
- It is not bit-identical to tap-at-a-time: each accumulator still adds its
  taps in a fixed order, just not index order. Last-bit differences only
  (-192.7 dBFS worst, on the 60 s benchmark clip).

To do: on the MacBook, time `declick_cli` best of seven, mono, with this
kernel against the tap-at-a-time one (both builds otherwise identical, AVX2
path). Keep the blocking if it pays there; revert to the simpler kernel if it
does not.
