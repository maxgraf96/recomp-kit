# Berkeley SoftFloat 3e (subset)

The 80-bit arithmetic behind the exact x87 model's out-of-line path
(`runtime/x87.c`). Precision control rounds through SoftFloat's
`extF80_roundingPrecision`, which keeps the x87's 15-bit exponent range at 24
and 53 bits.

- **Release:** Berkeley SoftFloat 3e by John R. Hauser, 2018-01-20.
- **Source:** <http://www.jhauser.us/arithmetic/SoftFloat-3e.zip>, SHA-256
  `21130ce885d35c1fe73fc1e1bf2244178167e05c6747cad5f450cc991714c746`.
- **Licence:** BSD 3-clause, in [COPYING.txt](COPYING.txt) (unmodified).

## What is here

Only the files the x87 layer needs, flattened into this directory and
byte-identical to the release:

- from `source/include/`: `internals.h`, `primitives.h`, `primitiveTypes.h`,
  `softfloat.h`, `softfloat_types.h`, `opts-GCC.h`;
- from `source/8086/` (the x87 specialisation: its default NaN is the x87
  indefinite, its invalid integer conversions return the integer indefinite):
  `specialize.h`, `softfloat_raiseFlags.c`, `s_commonNaNToF32UI.c`,
  `s_commonNaNToF64UI.c`, `s_extF80UIToCommonNaN.c`, `s_propagateNaNExtF80UI.c`;
- from `source/`: `extF80_add.c`, `extF80_sub.c`, `extF80_mul.c`,
  `extF80_div.c`, `extF80_sqrt.c`, `extF80_roundToInt.c`, `extF80_to_f32.c`,
  `extF80_to_f64.c`, `extF80_to_i64.c`, `i64_to_extF80.c`,
  `s_addMagsExtF80.c`, `s_subMagsExtF80.c`, `s_roundPackToExtF80.c`,
  `s_normRoundPackToExtF80.c`, `s_normSubnormalExtF80Sig.c`,
  `s_roundPackToF32.c`, `s_roundPackToF64.c`, `s_roundToI64.c`,
  `s_shiftRightJam128.c`, `s_approxRecipSqrt_1Ks.c`,
  `s_approxRecipSqrt32_1.c`, `softfloat_state.c`.

Two local files are not part of the release:

- `platform.h`: the build configuration SoftFloat expects each build to
  supply, based on upstream's `build/Linux-x86_64-GCC/platform.h`. It defines
  the options upstream passes on the command line, makes `INLINE` `static
  inline` (so an `-O0` build links) and makes the rounding state per thread.
- `softfloat_x87.c`: includes the sources above as one translation unit, the
  only one a build compiles.

The code uses only `<stdint.h>` and `<stdbool.h>` and builds with
`-ffreestanding`. To update it, replace the files above from a new release,
keep the two local files, and run `tools/recomp/tests/x87_conformance.py`.
