const fixuint = @import("fixuint.zig").fixuint;
const builtin = @import("builtin");
const linkage = if (builtin.is_test) builtin.GlobalLinkage.Internal else builtin.GlobalLinkage.LinkOnce;

export fn __fixunsdfti(a: f64) -> u128 {
    @setDebugSafety(this, builtin.is_test);
    @setGlobalLinkage(__fixunsdfti, linkage);
    return fixuint(f64, u128, a);
}

test "import fixunsdfti" {
    _ = @import("fixunsdfti_test.zig");
}
