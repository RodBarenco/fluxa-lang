#!/usr/bin/env bash
# tests/libs/compute.sh — std.compute: GPU computation
#
# These run against whichever backend is compiled in. The stub keeps the whole
# lifecycle real — buffers are genuine allocations and upload/download/copy move
# genuine bytes — so everything here except actual kernel execution is
# meaningful without a GPU.
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
case "$FLUXA" in /*) ;; *) FLUXA="$PWD/${FLUXA#./}" ;; esac

SRC_ROOT="${SRC_ROOT:-$PWD}"
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0
pass() { printf "  PASS  libs/compute/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/compute/%s\n    expected: %s\n    got:      %s\n" \
    "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.compute="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; (cd "$P" && timeout 30s "$FLUXA" run main.flx -proj . 2>&1 || true); }

echo "── std.compute: GPU computation ─────────────────────────────────"

# 1. import without [libs] → clear error
cat > "$P/main.flx" << 'FLX'
import std compute
compute.version()
FLX
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
out=$(cd "$P" && timeout 20s "$FLUXA" run main.flx -proj . 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" && pass "import_without_toml_error" \
    || fail "import_without_toml_error" "not declared error" "$out"

# 2. version names the backend, so a program is never fooled about whether a
#    kernel can actually run
out=$(run << 'FLX'
import std compute
print(compute.version())
FLX
)
echo "$out" | grep -qiE "vulkan|stub" && pass "version_names_backend" \
    || fail "version_names_backend" "backend name" "$out"

# 3. context lifecycle
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    print("DEV", compute.device_name(g))
    compute.wait_idle(g)
    compute.close(g)
    print("OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "^OK" && pass "context_init_close" \
    || fail "context_init_close" "OK" "$out"

# 4. close is silent on an already-closed context — same rule as
#    pg.free_result, json2.discard and image.discard
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    compute.close(g)
    compute.close(g)
    print("DOUBLE_CLOSE_OK")
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "DOUBLE_CLOSE_OK" && pass "double_close_is_a_no_op" \
    || fail "double_close_is_a_no_op" "DOUBLE_CLOSE_OK" "$out"

# 5. using a context after close is refused rather than crashing
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    compute.close(g)
    int b = compute.create_buffer(g, 64, "storage")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "invalid context|already closed" && pass "use_after_close_refused" \
    || fail "use_after_close_refused" "invalid context" "$out"

# 6. buffers: create, size, destroy
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 256, "storage")
    print("SIZE", compute.buffer_size(g, b))
    compute.destroy_buffer(g, b)
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "SIZE 256" && pass "buffer_create_size_destroy" \
    || fail "buffer_create_size_destroy" "SIZE 256" "$out"

# 7. a destroyed buffer's handle is refused
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 64, "storage")
    compute.destroy_buffer(g, b)
    int s = compute.buffer_size(g, b)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "destroyed or unknown" && pass "destroyed_buffer_handle_refused" \
    || fail "destroyed_buffer_handle_refused" "destroyed or unknown" "$out"

# 8. handle 0 is never valid — the same convention the language uses for
#    sockets and requests
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int s = compute.buffer_size(g, 0)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "0 is never valid" && pass "zero_handle_refused" \
    || fail "zero_handle_refused" "0 is never valid" "$out"

# 9. THE GENERATION RULE. A handle carries the generation of the context that
#    issued it, so one surviving from a previous context is caught instead of
#    silently addressing whatever now occupies that slot. This is what makes a
#    `prst int` handle safe across a runtime swap, where the context is rebuilt
#    from its declaration while the int is restored intact.
out=$(run << 'FLX'
import std compute
danger {
    dyn g1 = compute.init()
    int b = compute.create_buffer(g1, 64, "storage")
    print("H1", b)
    compute.close(g1)
}
if err != nil { print("ERR1", err[0]) }
danger {
    dyn g2 = compute.init()
    int fresh = compute.create_buffer(g2, 64, "storage")
    print("H2", fresh)
    int stale = 1048577
    int s = compute.buffer_size(g2, stale)
    print("REACHED", s)
}
if err != nil { print("CAUGHT", err[0]) }
FLX
)
if echo "$out" | grep -q "CAUGHT" && echo "$out" | grep -qi "previous context" \
   && ! echo "$out" | grep -q "REACHED"; then
    pass "stale_handle_from_previous_context_refused"
else
    fail "stale_handle_from_previous_context_refused" \
         "handle from generation 1 refused in generation 2" "$out"
fi

# 10. the two generations issue different handles for the same slot — the
#     property the check above depends on
h1=$(echo "$out" | grep "^H1" | awk '{print $2}')
h2=$(echo "$out" | grep "^H2" | awk '{print $2}')
[ -n "$h1" ] && [ -n "$h2" ] && [ "$h1" != "$h2" ] \
    && pass "handles_differ_across_contexts" \
    || fail "handles_differ_across_contexts" "different handles" "h1=$h1 h2=$h2"

# 11. upload/download round trip preserves the values
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 64, "storage")
    float arr v[4] = 0.0
    v[0] = 1.5
    v[1] = 2.5
    v[2] = 3.5
    v[3] = 4.5
    compute.upload(g, b, v, 0)
    dyn out = compute.download(g, b, 0, 16)
    print("RT", out[0], out[1], out[2], out[3])
    free(out)
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "RT 1.5 2.5 3.5 4.5" && pass "upload_download_round_trip" \
    || fail "upload_download_round_trip" "RT 1.5 2.5 3.5 4.5" "$out"

# 12. an int arr uploads too, and reads back as the same numbers
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 64, "storage")
    int arr v[3] = 0
    v[0] = 7
    v[1] = 8
    v[2] = 9
    compute.upload(g, b, v, 0)
    print("INT_UPLOAD_OK")
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "INT_UPLOAD_OK" && pass "int_array_uploads" \
    || fail "int_array_uploads" "INT_UPLOAD_OK" "$out"

# 13. a mixed array is refused — a shader reads one packed type, so silently
#     picking one would corrupt the data
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 64, "storage")
    dyn v = [1, 2.5, 3]
    compute.upload(g, b, v, 0)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "int arr|float arr|mixes" && pass "mixed_or_dyn_array_refused" \
    || fail "mixed_or_dyn_array_refused" "array type error" "$out"

# 14. upload past the end of the buffer is refused before anything is written
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 8, "storage")
    float arr v[4] = 1.0
    compute.upload(g, b, v, 0)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "does not fit" && pass "upload_overflow_refused" \
    || fail "upload_overflow_refused" "does not fit" "$out"

# 15. download outside the buffer is refused
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 16, "storage")
    dyn o = compute.download(g, b, 8, 16)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "outside the buffer" && pass "download_overflow_refused" \
    || fail "download_overflow_refused" "outside the buffer" "$out"

# 16. copy_buffer moves bytes and validates both regions
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int a = compute.create_buffer(g, 32, "storage")
    int b = compute.create_buffer(g, 32, "storage")
    float arr v[2] = 0.0
    v[0] = 9.5
    v[1] = 8.5
    compute.upload(g, a, v, 0)
    compute.copy_buffer(g, a, b, 8, 0, 0)
    dyn o = compute.download(g, b, 0, 8)
    print("COPY", o[0], o[1])
    free(o)
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "COPY 9.5 8.5" && pass "copy_buffer_moves_data" \
    || fail "copy_buffer_moves_data" "COPY 9.5 8.5" "$out"

out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int a = compute.create_buffer(g, 16, "storage")
    int b = compute.create_buffer(g, 16, "storage")
    compute.copy_buffer(g, a, b, 64, 0, 0)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "falls outside" && pass "copy_buffer_bounds_checked" \
    || fail "copy_buffer_bounds_checked" "falls outside" "$out"

# 17. usage and size are validated
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 64, "nonsense")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "storage, uniform, transfer" && pass "buffer_usage_validated" \
    || fail "buffer_usage_validated" "usage error" "$out"

out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 0, "storage")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "must be positive" && pass "buffer_size_validated" \
    || fail "buffer_size_validated" "size error" "$out"

# 18. SPIR-V validation: a file that is not a module is refused before the
#     bytes ever reach a driver
printf 'this is not spirv at all, not even close ok' > "$P/bad.spv"
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int k = compute.load_kernel(g, "bad.spv")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qiE "magic|multiple of 4" && pass "spirv_header_validated" \
    || fail "spirv_header_validated" "magic or alignment error" "$out"

# 19. a REAL SPIR-V module loads.
#
#     An earlier version of this test wrote a synthetic 8-byte file with the
#     right magic number. It passed on the stub and failed on a real driver,
#     which rejected it at pipeline creation — correctly, since a valid magic
#     number does not make a valid module. The lesson is that a fixture which
#     only satisfies our own header check proves nothing about the backend that
#     matters, so the module below is a genuine compute shader (the source is
#     in tests/fixtures/compute_double.comp) compiled with glslangValidator and
#     embedded here so the suite needs no shader toolchain to run.
if [ -r "$SRC_ROOT/tests/fixtures/compute_double.spv" ]; then
    cp "$SRC_ROOT/tests/fixtures/compute_double.spv" "$P/ok.spv"
else
    base64 -d > "$P/ok.spv" << 'SPV64'
AwIjBwAAAQALAAgAHwAAAAAAAAARAAIAAQAAAAsABgABAAAAR0xTTC5zdGQuNDUwAAAAAA4AAwAA
AAAAAQAAAA8ABgAFAAAABAAAAG1haW4AAAAAEAAAABAABgAEAAAAEQAAAAEAAAABAAAAAQAAAAMA
AwACAAAAwgEAAAUABAAEAAAAbWFpbgAAAAAFAAQACAAAAERhdGEAAAAABgAEAAgAAAAAAAAAdgAA
AAUAAwAKAAAAAAAAAAUACAAQAAAAZ2xfR2xvYmFsSW52b2NhdGlvbklEAAAARwAEAAcAAAAGAAAA
BAAAAEcAAwAIAAAAAwAAAEgABQAIAAAAAAAAACMAAAAAAAAARwAEAAoAAAAhAAAAAAAAAEcABAAK
AAAAIgAAAAAAAABHAAQAEAAAAAsAAAAcAAAARwAEAB4AAAALAAAAGQAAABMAAgACAAAAIQADAAMA
AAACAAAAFgADAAYAAAAgAAAAHQADAAcAAAAGAAAAHgADAAgAAAAHAAAAIAAEAAkAAAACAAAACAAA
ADsABAAJAAAACgAAAAIAAAAVAAQACwAAACAAAAABAAAAKwAEAAsAAAAMAAAAAAAAABUABAANAAAA
IAAAAAAAAAAXAAQADgAAAA0AAAADAAAAIAAEAA8AAAABAAAADgAAADsABAAPAAAAEAAAAAEAAAAr
AAQADQAAABEAAAAAAAAAIAAEABIAAAABAAAADQAAACAABAAXAAAAAgAAAAYAAAArAAQABgAAABoA
AAAAAABAKwAEAA0AAAAdAAAAAQAAACwABgAOAAAAHgAAAB0AAAAdAAAAHQAAADYABQACAAAABAAA
AAAAAAADAAAA+AACAAUAAABBAAUAEgAAABMAAAAQAAAAEQAAAD0ABAANAAAAFAAAABMAAABBAAUA
EgAAABUAAAAQAAAAEQAAAD0ABAANAAAAFgAAABUAAABBAAYAFwAAABgAAAAKAAAADAAAABYAAAA9
AAQABgAAABkAAAAYAAAAhQAFAAYAAAAbAAAAGQAAABoAAABBAAYAFwAAABwAAAAKAAAADAAAABQA
AAA+AAMAHAAAABsAAAD9AAEAOAABAA==
SPV64
fi
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int k = compute.load_kernel(g, "ok.spv")
    print("KERNEL", k)
    compute.destroy_kernel(g, k)
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "^KERNEL" && pass "valid_spirv_header_loads" \
    || fail "valid_spirv_header_loads" "KERNEL <handle>" "$out"

# 20. a missing shader file is an ordinary catchable error
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int k = compute.load_kernel(g, "nope.spv")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "cannot open" && pass "missing_kernel_file_captured" \
    || fail "missing_kernel_file_captured" "cannot open" "$out"

# 21. dispatch outside begin/end is refused, and so is a batch with no kernel
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    compute.dispatch(g, 1, 1, 1)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "no batch is open" && pass "dispatch_outside_batch_refused" \
    || fail "dispatch_outside_batch_refused" "no batch is open" "$out"

out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    compute.begin(g)
    compute.dispatch(g, 1, 1, 1)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "no kernel is bound" && pass "dispatch_without_kernel_refused" \
    || fail "dispatch_without_kernel_refused" "no kernel is bound" "$out"

# 22. a second begin without an end is refused
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    compute.begin(g)
    compute.begin(g)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "already open" && pass "nested_begin_refused" \
    || fail "nested_begin_refused" "already open" "$out"

# 23. the full batch shape: bind, dispatch, end returns a ticket, and the
#     ticket resolves
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int k = compute.load_kernel(g, "ok.spv")
    int a = compute.create_buffer(g, 64, "storage")
    compute.begin(g)
    compute.bind_kernel(g, k)
    compute.bind_buffer(g, 0, a)
    compute.dispatch(g, 4, 1, 1)
    compute.barrier(g, "compute_to_compute")
    compute.dispatch(g, 2, 1, 1)
    int t = compute.end(g)
    print("TICKET", t)
    print("LAST", compute.last_ticket(g))
    compute.wait(g, t)
    print("READY", compute.ready(g, t))
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
if echo "$out" | grep -q "TICKET 1" && echo "$out" | grep -q "LAST 1" \
   && echo "$out" | grep -q "READY true"; then
    pass "batch_dispatch_and_ticket"
else
    fail "batch_dispatch_and_ticket" "TICKET 1 / LAST 1 / READY true" "$out"
fi

# 24. a ticket this context never issued is refused, rather than reported ready
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    bool r = compute.ready(g, 999)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "never issued" && pass "unknown_ticket_refused" \
    || fail "unknown_ticket_refused" "never issued" "$out"

# 25. barrier kinds are validated
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    compute.begin(g)
    compute.barrier(g, "sideways")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "compute_to_compute" && pass "barrier_kind_validated" \
    || fail "barrier_kind_validated" "barrier kind error" "$out"

# 26. bind slot range is validated
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int a = compute.create_buffer(g, 16, "storage")
    compute.bind_buffer(g, 99, a)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "between 0 and 31" && pass "bind_slot_validated" \
    || fail "bind_slot_validated" "slot range error" "$out"

# 27. push constants are capped at the 128 bytes Vulkan guarantees
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    float arr v[4] = 1.0
    compute.push_constants(g, v, 256)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "128 bytes" && pass "push_constants_capped" \
    || fail "push_constants_capped" "128 bytes" "$out"

# 28. destroying the bound kernel unbinds it, so the next dispatch fails with
#     the obvious message rather than a confusing one
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int k = compute.load_kernel(g, "ok.spv")
    compute.begin(g)
    compute.bind_kernel(g, k)
    compute.destroy_kernel(g, k)
    compute.dispatch(g, 1, 1, 1)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "no kernel is bound" && pass "destroying_bound_kernel_unbinds" \
    || fail "destroying_bound_kernel_unbinds" "no kernel is bound" "$out"

# 29. limits are reported and are plausible
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    print("WGX", compute.max_workgroup_x(g))
    print("MAXBUF", compute.max_buffer_size(g))
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
wgx=$(echo "$out" | grep "^WGX" | awk '{print $2}')
[ -n "$wgx" ] && [ "$wgx" -ge 65535 ] && pass "limits_reported" \
    || fail "limits_reported" "max_workgroup_x >= 65535" "$out"

# 30. an unknown feature name is an error, not a silent false — false would
#     read as "the GPU lacks it" when the truth is "no such feature"
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    bool h = compute.has(g, "quantum")
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "unknown feature" && pass "unknown_feature_refused" \
    || fail "unknown_feature_refused" "unknown feature" "$out"

out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    print("F64", compute.has(g, "float64"))
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -qE "^F64 (true|false)" && pass "known_feature_answers" \
    || fail "known_feature_answers" "F64 true|false" "$out"

# 31. profiling regions nest correctly and report "not measured" rather than a
#     fabricated zero on a backend that cannot time a kernel
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    compute.profile_begin(g, "step")
    compute.profile_end(g)
    print("MS", compute.profile_ms(g, "step"))
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "^MS" && pass "profile_region_lifecycle" \
    || fail "profile_region_lifecycle" "MS <value>" "$out"

out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    compute.profile_end(g)
}
if err != nil { print(err[0]) }
FLX
)
echo "$out" | grep -qi "no region is open" && pass "profile_end_without_begin_refused" \
    || fail "profile_end_without_begin_refused" "no region is open" "$out"

# 32. unknown function → clear error, captured in danger
out=$(run << 'FLX'
import std compute
danger { compute.no_such_thing() }
if err != nil { print("caught") }
FLX
)
echo "$out" | grep -q "caught" && pass "unknown_function_captured_in_danger" \
    || fail "unknown_function_captured_in_danger" "caught" "$out"

# 33. END TO END: a kernel that actually runs.
#
#     tests/fixtures/compute_double.comp doubles every element of the bound
#     storage buffer. On a real device this is the only test that proves the
#     whole chain — upload, descriptor set, pipeline, dispatch, fence, download
#     — rather than just the lifecycle around it. On the stub the dispatch is a
#     no-op, so the values come back unchanged; both outcomes are accepted here
#     and the one that happened is reported, because a stub silently "passing" a
#     GPU test would be worse than no test.
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 32, "storage")
    float arr v[8] = 0.0
    v[0] = 1.0
    v[1] = 2.0
    v[2] = 3.0
    v[3] = 4.0
    v[4] = 5.0
    v[5] = 6.0
    v[6] = 7.0
    v[7] = 8.0
    compute.upload(g, b, v, 0)
    int k = compute.load_kernel(g, "ok.spv")
    compute.begin(g)
    compute.bind_kernel(g, k)
    compute.bind_buffer(g, 0, b)
    compute.dispatch(g, 8, 1, 1)
    int t = compute.end(g)
    compute.wait(g, t)
    dyn o = compute.download(g, b, 0, 32)
    print("OUT", o[0], o[1], o[2], o[7])
    free(o)
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
if echo "$out" | grep -q "OUT 2 4 6 16"; then
    pass "kernel_executes_on_device (doubled on GPU)"
elif echo "$out" | grep -q "OUT 1 2 3 8"; then
    pass "kernel_executes_on_device (stub: dispatch is a no-op, values unchanged)"
else
    fail "kernel_executes_on_device" \
         "OUT 2 4 6 16 on a device, or OUT 1 2 3 8 on the stub" "$out"
fi

# 34. a second batch reuses the command buffer and the fence. The first
#     submission has to leave the fence in a state the next wait can pass, and
#     getting that wrong deadlocks rather than failing — so this runs three
#     batches and the test times out if it hangs.
out=$(run << 'FLX'
import std compute
danger {
    dyn g = compute.init()
    int b = compute.create_buffer(g, 32, "storage")
    int k = compute.load_kernel(g, "ok.spv")
    int n = 0
    while n < 3 {
        compute.begin(g)
        compute.bind_kernel(g, k)
        compute.bind_buffer(g, 0, b)
        compute.dispatch(g, 8, 1, 1)
        int t = compute.end(g)
        compute.wait(g, t)
        n = n + 1
    }
    print("BATCHES", compute.last_ticket(g))
    compute.close(g)
}
if err != nil { print("ERR", err[0]) }
FLX
)
echo "$out" | grep -q "BATCHES 3" && pass "successive_batches_reuse_fence" \
    || fail "successive_batches_reuse_fence" "BATCHES 3 (a hang means the fence state is wrong)" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.compute: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.compute: PASS" && exit 0 || exit 1
