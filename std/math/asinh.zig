// Special Cases:
//
// - asinh(+-0)   = +-0
// - asinh(+-inf) = +-inf
// - asinh(nan)   = nan

const math = @import("index.zig");
const assert = @import("../debug.zig").assert;

pub fn asinh(x: var) -> @typeOf(x) {
    const T = @typeOf(x);
    switch (T) {
        f32 => @inlineCall(asinh32, x),
        f64 => @inlineCall(asinh64, x),
        else => @compileError("asinh not implemented for " ++ @typeName(T)),
    }
}

// asinh(x) = sign(x) * log(|x| + sqrt(x * x + 1)) ~= x - x^3/6 + o(x^5)
fn asinh32(x: f32) -> f32 {
    const u = @bitCast(u32, x);
    const i = u & 0x7FFFFFFF;
    const s = i >> 31;

    var rx = @bitCast(f32, i); // |x|

    // TODO: Shouldn't need this explicit check.
    if (math.isNegativeInf(x)) {
        return x;
    }

    // |x| >= 0x1p12 or inf or nan
    if (i >= 0x3F800000 + (12 << 23)) {
        rx = math.ln(rx) + 0.69314718055994530941723212145817656;
    }
    // |x| >= 2
    else if (i >= 0x3F800000 + (1 << 23)) {
        rx = math.ln(2 * x + 1 / (math.sqrt(x * x + 1) + x));
    }
    // |x| >= 0x1p-12, up to 1.6ulp error
    else if (i >= 0x3F800000 - (12 << 23)) {
        rx = math.log1p(x + x * x / (math.sqrt(x * x + 1) + 1));
    }
    // |x| < 0x1p-12, inexact if x != 0
    else {
        math.forceEval(x + 0x1.0p120);
    }

    if (s != 0) -rx else rx
}

fn asinh64(x: f64) -> f64 {
    const u = @bitCast(u64, x);
    const e = (u >> 52) & 0x7FF;
    const s = u >> 63;

    var rx = @bitCast(f64, u & (@maxValue(u64) >> 1)); // |x|

    if (math.isNegativeInf(x)) {
        return x;
    }

    // |x| >= 0x1p26 or inf or nan
    if (e >= 0x3FF + 26) {
        rx = math.ln(rx) + 0.693147180559945309417232121458176568;
    }
    // |x| >= 2
    else if (e >= 0x3FF + 1) {
        rx = math.ln(2 * x + 1 / (math.sqrt(x * x + 1) + x));
    }
    // |x| >= 0x1p-12, up to 1.6ulp error
    else if (e >= 0x3FF - 26) {
        rx = math.log1p(x + x * x / (math.sqrt(x * x + 1) + 1));
    }
    // |x| < 0x1p-12, inexact if x != 0
    else {
        math.forceEval(x + 0x1.0p120);
    }

    if (s != 0) -rx else rx
}

test "math.asinh" {
    assert(asinh(f32(0.0)) == asinh32(0.0));
    assert(asinh(f64(0.0)) == asinh64(0.0));
}

test "math.asinh32" {
    const epsilon = 0.000001;

    assert(math.approxEq(f32, asinh32(0.0), 0.0, epsilon));
    assert(math.approxEq(f32, asinh32(0.2), 0.198690, epsilon));
    assert(math.approxEq(f32, asinh32(0.8923), 0.803133, epsilon));
    assert(math.approxEq(f32, asinh32(1.5), 1.194763, epsilon));
    assert(math.approxEq(f32, asinh32(37.45), 4.316332, epsilon));
    assert(math.approxEq(f32, asinh32(89.123), 5.183196, epsilon));
    assert(math.approxEq(f32, asinh32(123123.234375), 12.414088, epsilon));
}

test "math.asinh64" {
    const epsilon = 0.000001;

    assert(math.approxEq(f64, asinh64(0.0), 0.0, epsilon));
    assert(math.approxEq(f64, asinh64(0.2), 0.198690, epsilon));
    assert(math.approxEq(f64, asinh64(0.8923), 0.803133, epsilon));
    assert(math.approxEq(f64, asinh64(1.5), 1.194763, epsilon));
    assert(math.approxEq(f64, asinh64(37.45), 4.316332, epsilon));
    assert(math.approxEq(f64, asinh64(89.123), 5.183196, epsilon));
    assert(math.approxEq(f64, asinh64(123123.234375), 12.414088, epsilon));
}

test "math.asinh32.special" {
    assert(asinh32(0.0) == 0.0);
    assert(asinh32(-0.0) == -0.0);
    assert(math.isPositiveInf(asinh32(math.inf(f32))));
    assert(math.isNegativeInf(asinh32(-math.inf(f32))));
    assert(math.isNan(asinh32(math.nan(f32))));
}

test "math.asinh64.special" {
    assert(asinh64(0.0) == 0.0);
    assert(asinh64(-0.0) == -0.0);
    assert(math.isPositiveInf(asinh64(math.inf(f64))));
    assert(math.isNegativeInf(asinh64(-math.inf(f64))));
    assert(math.isNan(asinh64(math.nan(f64))));
}
