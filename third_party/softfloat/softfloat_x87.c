/* softfloat_x87.c - the part of SoftFloat 3e the exact x87 layer uses, as one
 * translation unit.
 *
 * Not part of the upstream release. Every file included here is upstream's,
 * unmodified (see README.md); the 8086 specialisation supplies the x87
 * default NaN and integer indefinite. Building them as one unit keeps the
 * runtime's source lists and the translator test harnesses to a single extra
 * file. runtime/x87.c is the only caller. */
#include "platform.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-label"
#endif

#include "softfloat_state.c"
#include "softfloat_raiseFlags.c"

#include "s_shiftRightJam128.c"
#include "s_approxRecipSqrt_1Ks.c"
#include "s_approxRecipSqrt32_1.c"

#include "s_commonNaNToF32UI.c"
#include "s_commonNaNToF64UI.c"
#include "s_extF80UIToCommonNaN.c"
#include "s_propagateNaNExtF80UI.c"

#include "s_normSubnormalExtF80Sig.c"
#include "s_roundPackToExtF80.c"
#include "s_normRoundPackToExtF80.c"
#include "s_roundPackToF32.c"
#include "s_roundPackToF64.c"
#include "s_roundToI64.c"
#include "s_addMagsExtF80.c"
#include "s_subMagsExtF80.c"

#include "extF80_add.c"
#include "extF80_sub.c"
#include "extF80_mul.c"
#include "extF80_div.c"
#include "extF80_sqrt.c"
#include "extF80_roundToInt.c"
#include "extF80_to_f32.c"
#include "extF80_to_f64.c"
#include "extF80_to_i64.c"
#include "i64_to_extF80.c"
