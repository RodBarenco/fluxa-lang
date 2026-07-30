#!/usr/bin/env bash
set -euo pipefail

# raylib 6.0, pinned to the commit referenced by the upstream 6.0 tag.
RAYLIB_VERSION="6.0"
RAYLIB_COMMIT="dbc56a87da87d973a9c5baa4e7438a9d20121d28"
SOURCE_DIR="${WINDOWS_RAYLIB_SOURCE_DIR:-.deps/raylib-src}"
PREFIX="${WINDOWS_RAYLIB_STATIC_PREFIX:-.deps/raylib-static}"
GIT_BIN="${GIT_BIN:-git}"

if ! command -v "$GIT_BIN" >/dev/null 2>&1; then
    if [ -x "/c/Program Files/Git/cmd/git.exe" ]; then
        GIT_BIN="/c/Program Files/Git/cmd/git.exe"
    else
        echo "git is required to fetch the pinned raylib source" >&2
        exit 1
    fi
fi

if [ ! -d "$SOURCE_DIR/.git" ]; then
    mkdir -p "$(dirname "$SOURCE_DIR")"
    "$GIT_BIN" clone --filter=blob:none --no-checkout \
        https://github.com/raysan5/raylib.git "$SOURCE_DIR"
fi

"$GIT_BIN" -C "$SOURCE_DIR" fetch --depth 1 origin "$RAYLIB_COMMIT"
"$GIT_BIN" -C "$SOURCE_DIR" checkout --detach --force "$RAYLIB_COMMIT"

# raylib 6.0's native Win32 backend uses per-monitor DPI declarations that
# MinGW exposes only when the Windows 10 API level is selected.
python3 - "$SOURCE_DIR/src/Makefile" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = "CFLAGS += -DUNICODE"
new = "CFLAGS += -DUNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00"
if new not in text:
    if old not in text:
        raise SystemExit("unexpected raylib Makefile: Windows CFLAGS anchor missing")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY

rm -f "$SOURCE_DIR"/src/*.o "$SOURCE_DIR"/src/libraylib.a
make -C "$SOURCE_DIR/src" \
    PLATFORM=PLATFORM_DESKTOP_WIN32 \
    RAYLIB_LIBTYPE=STATIC

mkdir -p "$PREFIX/include" "$PREFIX/lib"
cp "$SOURCE_DIR/src/raylib.h" "$PREFIX/include/"
cp "$SOURCE_DIR/src/raymath.h" "$PREFIX/include/"
cp "$SOURCE_DIR/src/rlgl.h" "$PREFIX/include/"
cp "$SOURCE_DIR/src/libraylib.a" "$PREFIX/lib/"

actual_commit="$("$GIT_BIN" -C "$SOURCE_DIR" rev-parse HEAD)"
test "$actual_commit" = "$RAYLIB_COMMIT"
printf 'raylib %s static Win32 ready at %s\n' "$RAYLIB_VERSION" "$PREFIX"
