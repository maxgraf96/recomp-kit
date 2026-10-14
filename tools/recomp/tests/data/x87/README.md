# x87 hardware vectors

What `x87_conformance.py` checks the translated x87 against: 3,770
instruction vectors and what a real x87 made of each.

- `insn-subset.jsonl`: the vectors.
- `insn-subset-hw.jsonl`: the hardware's results, recorded on an Intel Core
  Ultra 9 275HX (first line: the recording's metadata).

They are synthetic: a seeded generator (seed 1) draws the operands from
classes - random doubles, floats and 64-bit significands, values of a game's
typical magnitudes, exact and near rounding midpoints at 24 and 53 bits,
results outside the double's range, NaNs, infinities, zeros and denormals,
integer limits and ties - over six control words (0x027f, 0x007f, 0x037f,
0x0e7f, 0x067f, 0x0a7f). They contain no game code or data. This subset takes
3 vectors of every operation x control word x class, 8 of the special, tie,
range and limit classes and 2 of the transcendental ones, from 187,182.

## Edge vectors

- `insn-edge.jsonl`, `insn-edge-model.jsonl`: 1,044 vectors for what the
  recorded classes miss - products that a double rounds up to DBL_MIN from
  below, PC=24 sums whose double result lies exactly on a 24-bit midpoint,
  quotients beside DBL_MIN - and for four forms the recording does not use
  (`fsubr_m64`, `fcom_m64`, `fild_m16`, `fild_m32`). Their results come from
  the SDM reference model (exact rational arithmetic rounded per the control
  word) that equals the hardware on every recorded vector, 1.15 million of
  them. A hardware recording of the same vectors would replace it.

## Format

JSON Lines. A vector:

```json
{"id": "ar-000001", "kind": "x87", "op": "fadd", "cw": "027f",
 "a": "x80:400be6b09ec78b56f800", "b": "x80:bfcb8f17f20a61a1e000", "tag": "arith/rand53"}
```

- `op`: a key of `OPS` in `x87_conformance.py`.
- `a`, `b`, `c`: ST(0), ST(1), ST(2) before the operation.
- `m`: the memory input of the memory forms.
- Typed values are hex of the bit pattern: `x80:SSSSMMMMMMMMMMMMMMMM` (sign
  and exponent, then the 64-bit significand with its integer bit), `f32:`,
  `f64:`, `i16:`, `i32:`, `i64:`.

A result, matched by `id`:

```json
{"id": "ar-000001", "out": ["x80:...", "x80:..."], "sw": "3220", "sw_end": "0020"}
```

- `out`: every register left on the stack afterwards, ST(0) first.
- `mem`: the memory output of a store.
- `sw`: the status word straight after the operation; `sw_end` after the
  registers were stored.

## Larger corpora

The same generator and recorder produce corpora of any size; a 1,085,508
vector one (seed 2) is the reference for the full check. Run it with

```sh
.venv/bin/python tools/recomp/tests/x87_conformance.py --vectors VECTORS.jsonl --results RESULTS.jsonl
```

or set `RECOMP_X87_VECTORS` and `RECOMP_X87_RESULTS` for the pytest wrapper.
