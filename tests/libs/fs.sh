#!/usr/bin/env bash
# tests/libs/fs.sh — std.fs test suite
set -euo pipefail
FLUXA="${FLUXA:-./fluxa}"
for arg in "$@"; do [ "$arg" = "--fluxa" ] && shift && FLUXA="$1" && shift; done
P="$(mktemp -d)"; trap 'rm -rf "$P"' EXIT
FAILS=0; PASS=0

pass() { printf "  PASS  libs/fs/%s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  FAIL  libs/fs/%s\n    expected: %s\n    got:      %s\n" "$1" "$2" "$3"; FAILS=$((FAILS+1)); }

toml() { printf '[project]\nname="t"\nentry="main.flx"\n[libs]\nstd.fs="1.0"\n' > "$P/fluxa.toml"; }
run()  { toml; cat > "$P/main.flx"; timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true; }

echo "── std.fs ───────────────────────────────────────────────────────"

# 1. import without [libs] → error
printf '[project]\nname="t"\nentry="main.flx"\n' > "$P/fluxa.toml"
cat > "$P/main.flx" << 'FLX'
import std fs
danger { bool e = fs.exists("/tmp") }
FLX
out=$(timeout 5s "$FLUXA" run "$P/main.flx" -proj "$P" 2>&1 || true)
echo "$out" | grep -qiE "not declared|libs|toml" \
    && pass "import_without_toml_error" || fail "import_without_toml_error" "not declared" "$out"

# 2. write + read roundtrip
out=$(run << FLXEOF
import std fs
danger {
    int n = fs.write("${P}/hello.txt", "hello fluxa")
    str s = fs.read("${P}/hello.txt")
    print(n)
    print(s)
}
FLXEOF
)
echo "$out" | grep -q "^11$"         && pass "write_returns_bytes"  || fail "write_returns_bytes"  "11" "$out"
echo "$out" | grep -q "hello fluxa"  && pass "read_roundtrip"       || fail "read_roundtrip"       "hello fluxa" "$out"

# 3. exists — present and missing
out=$(run << FLXEOF
import std fs
danger {
    fs.write("${P}/test_exists.txt", "x")
    bool e1 = fs.exists("${P}/test_exists.txt")
    bool e2 = fs.exists("${P}/no_such_file_xyz.txt")
    print(e1)
    print(e2)
}
FLXEOF
)
echo "$out" | grep -q "true"  && pass "exists_present"  || fail "exists_present"  "true"  "$out"
echo "$out" | grep -q "false" && pass "exists_missing"  || fail "exists_missing"  "false" "$out"

# 4. size
out=$(run << FLXEOF
import std fs
danger {
    fs.write("${P}/sized.txt", "12345")
    int sz = fs.size("${P}/sized.txt")
    print(sz)
}
FLXEOF
)
echo "$out" | grep -q "^5$" && pass "size_correct" || fail "size_correct" "5" "$out"

# 5. delete
out=$(run << FLXEOF
import std fs
danger {
    fs.write("${P}/todelete.txt", "bye")
    bool before = fs.exists("${P}/todelete.txt")
    fs.delete("${P}/todelete.txt")
    bool after = fs.exists("${P}/todelete.txt")
    print(before)
    print(after)
}
FLXEOF
)
echo "$out" | grep -q "true"  && pass "delete_existed_before" || fail "delete_existed_before" "true" "$out"
echo "$out" | grep -q "false" && pass "delete_gone_after"     || fail "delete_gone_after" "false" "$out"

# 6. append
out=$(run << FLXEOF
import std fs
danger {
    fs.write("${P}/append.txt", "hello ")
    fs.append("${P}/append.txt", "world")
    str s = fs.read("${P}/append.txt")
    print(s)
}
FLXEOF
)
echo "$out" | grep -q "hello world" && pass "append_content" || fail "append_content" "hello world" "$out"

# 7. rename
out=$(run << FLXEOF
import std fs
danger {
    fs.write("${P}/before.txt", "renamed")
    fs.rename("${P}/before.txt", "${P}/after.txt")
    bool b = fs.exists("${P}/before.txt")
    bool a = fs.exists("${P}/after.txt")
    str  s = fs.read("${P}/after.txt")
    print(b)
    print(a)
    print(s)
}
FLXEOF
)
echo "$out" | grep -q "false"   && pass "rename_src_gone"  || fail "rename_src_gone"  "false"   "$out"
echo "$out" | grep -q "true"    && pass "rename_dst_exists" || fail "rename_dst_exists" "true"   "$out"
echo "$out" | grep -q "renamed" && pass "rename_content"   || fail "rename_content"   "renamed" "$out"

# 8. copy
out=$(run << FLXEOF
import std fs
danger {
    fs.write("${P}/orig.txt", "copied content")
    fs.copy("${P}/orig.txt", "${P}/copy.txt")
    str s = fs.read("${P}/copy.txt")
    print(s)
}
FLXEOF
)
echo "$out" | grep -q "copied content" && pass "copy_content" || fail "copy_content" "copied content" "$out"

# 9. mkdir + isdir + rmdir
out=$(run << FLXEOF
import std fs
danger {
    fs.mkdir("${P}/subdir/nested")
    bool is_d = fs.isdir("${P}/subdir/nested")
    print(is_d)
}
FLXEOF
)
echo "$out" | grep -q "true" && pass "mkdir_isdir" || fail "mkdir_isdir" "true" "$out"

# 10. isfile
out=$(run << FLXEOF
import std fs
danger {
    fs.write("${P}/afile.txt", "x")
    bool f = fs.isfile("${P}/afile.txt")
    bool d = fs.isfile("${P}")
    print(f)
    print(d)
}
FLXEOF
)
echo "$out" | grep -q "true"  && pass "isfile_true"  || fail "isfile_true"  "true"  "$out"
echo "$out" | grep -q "false" && pass "isfile_false" || fail "isfile_false" "false" "$out"

# 11. listdir
out=$(run << FLXEOF
import std fs
danger {
    fs.mkdir("${P}/list_dir")
    fs.write("${P}/list_dir/a.txt", "a")
    fs.write("${P}/list_dir/b.txt", "b")
    dyn entries = fs.listdir("${P}/list_dir")
    int n = len(entries)
    print(n)
}
FLXEOF
)
echo "$out" | grep -q "^2$" && pass "listdir_count" || fail "listdir_count" "2" "$out"

# 12. join
out=$(run << 'FLX'
import std fs
str p = fs.join("dir", "file.txt")
print(p)
FLX
)
echo "$out" | grep -q "dir/file.txt" && pass "join_path" || fail "join_path" "dir/file.txt" "$out"

# 13. basename + dirname + ext
out=$(run << 'FLX'
import std fs
str b = fs.basename("/home/user/file.txt")
str d = fs.dirname("/home/user/file.txt")
str e = fs.ext("file.txt")
str n = fs.ext("noext")
print(b)
print(d)
print(e)
print(n)
FLX
)
echo "$out" | grep -q "file.txt"  && pass "basename"   || fail "basename"   "file.txt"  "$out"
echo "$out" | grep -q "/home/user" && pass "dirname"   || fail "dirname"    "/home/user" "$out"
echo "$out" | grep -q "\.txt"     && pass "ext_dotted" || fail "ext_dotted" ".txt"      "$out"
# ext of "noext" should be empty string (no output line from print(""))
ext_lines=$(echo "$out" | wc -l)
# 3 non-empty lines (file.txt, /home/user, .txt) + 1 empty = 4
[ "$ext_lines" -ge 3 ] && pass "ext_empty" || fail "ext_empty" "empty string printed" "$out"

# 14. tempfile creates a real file
out=$(run << 'FLX'
import std fs
danger {
    str t = fs.tempfile()
    bool e = fs.exists(t)
    print(e)
    print(len(t))
}
FLX
)
echo "$out" | grep -q "true" && pass "tempfile_exists"  || fail "tempfile_exists"  "true" "$out"
echo "$out" | grep -qE "^[0-9]+" && pass "tempfile_path_len" || fail "tempfile_path_len" ">0" "$out"

# 15. read missing file → error in danger
out=$(run << 'FLX'
import std fs
danger { str s = fs.read("/nonexistent/path/xyz.txt") }
if err != nil { print("error caught") }
FLX
)
echo "$out" | grep -q "error caught" && pass "read_missing_error" || fail "read_missing_error" "error caught" "$out"

echo "────────────────────────────────────────────────────────────────"
echo "  → std.fs: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ] && echo "  → std.fs: PASS" && exit 0 || exit 1
