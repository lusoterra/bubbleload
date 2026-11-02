#!/usr/bin/env bash
#
# bubbleload test suite.
#
# Everything runs with --dry-run, so a working sandbox is not required.  The
# only exception is the optional smoke test at the end, which is skipped when
# bwrap is not installed.
#
# usage: tests/run.sh path/to/bubbleload

set -uo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 path/to/bubbleload" >&2
    exit 2
fi
BIN=$(realpath "$1" 2>/dev/null) || BIN=$1
if [ ! -x "$BIN" ]; then
    echo "not executable: $BIN" >&2
    exit 2
fi

TESTDIR=$(cd "$(dirname "$0")" && pwd)
EXAMPLES=$(cd "$TESTDIR/.." && pwd)/examples
if [ ! -f "$EXAMPLES/generic.conf" ] || [ ! -f "$EXAMPLES/opencode.conf" ]; then
    echo "cannot find examples/ next to $TESTDIR" >&2
    exit 2
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/bubbleload-test.XXXXXX") || exit 2
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

HOME="$TMP/home"
export HOME
export XDG_CONFIG_HOME="$TMP/config"
CFG="$XDG_CONFIG_HOME/bubbleload"
PROJ="$TMP/project"
RW="$TMP/rw"
RO="$TMP/ro"
mkdir -p "$HOME" "$CFG" "$PROJ" "$RW" "$RO"
unset XDG_RUNTIME_DIR

cp "$EXAMPLES/generic.conf" "$CFG/generic.conf"
cp "$EXAMPLES/opencode.conf" "$CFG/opencode.conf"

cat >"$CFG/other.conf" <<'EOF'
die-with-parent
project
EOF

cat >"$CFG/prog.conf" <<'EOF'
hostname from-basename
project
EOF

cat >"$CFG/inner.conf" <<'EOF'
hostname inner-host
EOF

cat >"$CFG/outer.conf" <<'EOF'
hostname outer-host
include inner
raw --last-flag
project
EOF

cat >"$CFG/cycle-a.conf" <<'EOF'
hostname cycle-a
include cycle-b
EOF

cat >"$CFG/cycle-b.conf" <<'EOF'
hostname cycle-b
include cycle-a
EOF

cat >"$CFG/badvar.conf" <<'EOF'
hostname $NOPE
EOF

cat >"$CFG/badxdg.conf" <<'EOF'
dir $XDG_RUNTIME_DIR
EOF

cat >"$CFG/expand.conf" <<'EOF'
hostname host-$UID
dir $PROJECT/sub
raw --pwd $PWD
raw --tilde ~/x
raw --braced ${PROJECT}
EOF

cat >"$CFG/exec-args.conf" <<'EOF'
hostname exec-args
project
exec run-me --fixed
EOF

cat >"$CFG/plain.conf" <<'EOF'
die-with-parent
project
EOF

cat >"$CFG/extra-default.conf" <<'EOF'
hostname extra-default
project
EOF

cat >"$CFG/extra-explicit.conf" <<'EOF'
hostname extra-explicit
raw --first
extra-binds
project
EOF

cat >"$CFG/no-proj.conf" <<'EOF'
hostname no-proj
EOF

# ok1..ok9: 8 nested includes, just under the limit.
for n in 1 2 3 4 5 6 7 8 9; do
    if [ "$n" -lt 9 ]; then
        printf 'include ok%d\n' "$((n + 1))" >"$CFG/ok$n.conf"
    else
        printf 'hostname depth-end\n' >"$CFG/ok$n.conf"
    fi
done
# deep1..deep10: 9 nested includes, over the limit.
for n in 1 2 3 4 5 6 7 8 9 10; do
    if [ "$n" -lt 10 ]; then
        printf 'include deep%d\n' "$((n + 1))" >"$CFG/deep$n.conf"
    else
        printf 'hostname deep-end\n' >"$CFG/deep$n.conf"
    fi
done

cp "$CFG/other.conf" "$CFG/perm.conf"
chmod 0666 "$CFG/perm.conf"

passes=0
failures=0

fail_test() {
    local name=$1

    shift
    failures=$((failures + 1))
    printf 'FAIL %s\n' "$name"
    for line in "$@"; do
        printf '     %s\n' "$line"
    done
}

ok_test() {
    passes=$((passes + 1))
    printf 'ok   %s\n' "$1"
}

RC=0
OUT=""
ERR=""

run_raw() { # stdin-string, command...
    local input=$1

    shift
    printf '%s' "$input" | "$@" >"$TMP/stdout" 2>"$TMP/stderr"
    RC=$?
    OUT=$(cat "$TMP/stdout")
    ERR=$(cat "$TMP/stderr")
}

run_bin() { # stdin-string, bubbleload args...
    local input=$1

    shift
    run_raw "$input" "$BIN" "$@"
}

no_sanitizer_errors() {
    ! grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' \
        "$TMP/stderr"
}

# Shared precondition: exit code 0 and no sanitizer diagnostics.  Prints
# nothing on success so every test reports exactly one result.
require_ok() {
    local name=$1

    if [ "$RC" -ne 0 ]; then
        fail_test "$name" "expected exit 0, got $RC" "stderr: $ERR"
        return 1
    fi
    if ! no_sanitizer_errors; then
        fail_test "$name" "sanitizer reported an error:" "$ERR"
        return 1
    fi
    return 0
}

assert_success() {
    require_ok "$1" && ok_test "$1"
}

parse_words() {
    local line

    WORDS=()
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        eval "set -- $line"
        WORDS+=("$@")
    done <<<"$OUT"
}

assert_argv() { # name -- expected words...
    local name=$1

    shift
    [ "${1:-}" = "--" ] && shift
    local -a want=("$@")
    local i

    require_ok "$name" || return
    parse_words
    if [ "${#WORDS[@]}" -ne "${#want[@]}" ]; then
        fail_test "$name" "expected ${#want[@]} words, got ${#WORDS[@]}" \
            "got:  $(printf '%q ' "${WORDS[@]}")" \
            "want: $(printf '%q ' "${want[@]}")"
        return
    fi
    for ((i = 0; i < ${#want[@]}; i++)); do
        if [ "${WORDS[i]}" != "${want[i]}" ]; then
            fail_test "$name" "word $i: got '${WORDS[i]}', want '${want[i]}'" \
                "got:  $(printf '%q ' "${WORDS[@]}")" \
                "want: $(printf '%q ' "${want[@]}")"
            return
        fi
    done
    ok_test "$name"
}

assert_stdout_has() { # name fixed-string
    local name=$1 pat=$2

    require_ok "$name" || return
    if ! grep -qF -- "$pat" <<<"$OUT"; then
        fail_test "$name" "stdout does not contain '$pat'" "stdout: $OUT"
        return
    fi
    ok_test "$name"
}

assert_stdout_lacks() { # name fixed-string
    local name=$1 pat=$2

    require_ok "$name" || return
    if grep -qF -- "$pat" <<<"$OUT"; then
        fail_test "$name" "stdout unexpectedly contains '$pat'" "stdout: $OUT"
        return
    fi
    ok_test "$name"
}

assert_stderr_has() { # name fixed-string
    local name=$1 pat=$2

    require_ok "$name" || return
    if ! grep -qF -- "$pat" <<<"$ERR"; then
        fail_test "$name" "stderr does not contain '$pat'" "stderr: $ERR"
        return
    fi
    ok_test "$name"
}

assert_fail() { # name stderr-substring
    local name=$1 pat=$2

    if [ "$RC" -eq 0 ]; then
        fail_test "$name" "expected a non-zero exit, got 0" "stdout: $OUT"
        return
    fi
    if ! no_sanitizer_errors; then
        fail_test "$name" "sanitizer reported an error:" "$ERR"
        return
    fi
    if ! grep -qF -- "$pat" <<<"$ERR"; then
        fail_test "$name" "stderr does not contain '$pat'" "stderr: $ERR"
        return
    fi
    ok_test "$name"
}

echo "== option parsing =="

# -w before BINARY is a loader option, -w/--rw after it belong to the program.
run_bin "" -y -n -p other -P "$PROJ" -w "$RW" prog -w --rw
assert_argv "loader options stop at BINARY" -- \
    bwrap --die-with-parent \
    --bind "$RW" "$RW" \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" \
    prog -w --rw

run_bin "" -y -n -p other -P "$PROJ" prog -w "$RW" -P "$RO"
assert_argv "options after BINARY are never parsed by the loader" -- \
    bwrap --die-with-parent \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" \
    prog -w "$RW" -P "$RO"

run_bin "" -y -n -p other -P "$PROJ" -- prog -n -y
assert_argv "-- ends loader options explicitly" -- \
    bwrap --die-with-parent \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" \
    prog -n -y

run_bin "" -y -n -p other -P "$PROJ" -- -weird-binary
assert_argv "a BINARY that looks like an option, after --" -- \
    bwrap --die-with-parent \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" \
    -weird-binary

run_bin "" --bogus-option prog
assert_fail "unknown loader option is rejected" "unknown option"

run_bin "" -n
assert_fail "missing BINARY is rejected" "missing BINARY"

run_bin "" --help
assert_stdout_has "-h prints usage" "Usage: bubbleload"

run_bin "" -y -n -P "$PROJ" -w "$RW" -r "$RO" prog
assert_argv "--rw/--ro become --bind/--ro-bind" -- \
    bwrap --hostname from-basename \
    --ro-bind "$RO" "$RO" --bind "$RW" "$RW" \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" prog

echo "== profiles =="

run_bin "" -y -n -P "$PROJ" prog
assert_stdout_has "profile name defaults to basename(BINARY)" \
    "--hostname from-basename"

run_bin "" -y -n -P "$PROJ" -p inner prog
assert_stdout_has "-p overrides the derived profile name" "--hostname inner-host"

run_bin "" -y -n -P "$PROJ" -p outer prog
assert_argv "include is inlined at the include position" -- \
    bwrap --hostname outer-host --hostname inner-host --last-flag \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" prog

run_bin "" -y -n -P "$PROJ" -p cycle-a prog
assert_fail "include cycle is detected" "include cycle detected"

run_bin "" -y -n -P "$PROJ" -p ok1 prog
assert_stdout_has "8 nested includes are allowed" "--hostname depth-end"

run_bin "" -y -n -P "$PROJ" -p deep1 prog
assert_fail "include depth limit is enforced" "include depth limit"

run_bin "" -y -n -P "$PROJ" -p badvar prog
assert_fail "unknown variable is an error" 'unknown variable $NOPE'

run_bin "" -y -n -P "$PROJ" -p badxdg prog
assert_fail "unset known variable is an error" '$XDG_RUNTIME_DIR is not set'

run_bin "" -y -n -P "$PROJ" -p expand prog
assert_argv 'variables $PROJECT/$PWD/$UID/${..}/~ are expanded' -- \
    bwrap --hostname "host-$(id -u)" --dir "$PROJ/sub" \
    --pwd "$(pwd -P)" --tilde "$HOME/x" --braced "$PROJ" prog

run_bin "" -y -n -p ../../etc/passwd prog
assert_fail "path traversal in -p is rejected" "invalid profile name"

run_bin "" -y -n -p .hidden prog
assert_fail "leading dot in -p is rejected" "invalid profile name"

run_bin "" -y -n -p a/b prog
assert_fail "slash in -p is rejected" "invalid profile name"

if [ "$(id -u)" -ne 0 ]; then
    run_bin "" -y -n -P "$PROJ" -p perm prog
    assert_fail "group/world writable config is refused" "world writable"
fi

echo "== path safety =="

run_bin "" -y -n -P "$HOME" prog
assert_fail "project directory == \$HOME is refused" \
    'refusing to expose all of $HOME'

run_bin "" -y -n -P "$PROJ" -w "$HOME" prog
assert_fail "--rw directory == \$HOME is refused" \
    'refusing to expose all of $HOME'

run_bin "" -y -n -P "$PROJ" -w "$TMP/does-not-exist" prog
assert_fail "missing --rw path is refused" "-w/--rw"

run_bin "" -y -n -P "$TMP/does-not-exist" prog
assert_fail "missing -P path is refused" "-P/--project"

cat >"$CFG/cwd.conf" <<'EOF'
die-with-parent
cwd
EOF

echo "== cwd and home safety =="

run_bin "" -y -n -p cwd prog
assert_stdout_has "the cwd directive exposes the current directory" \
    "--bind $(pwd -P) $(pwd -P) --chdir $(pwd -P)"

run_bin "" -y -n -p plain prog
assert_stdout_lacks "cwd is not exposed by default" \
    "--bind $PROJ $PROJ --chdir $PROJ"

run_bin "" -y -n --cwd prog
assert_stdout_has "--cwd exposes the current directory" \
    "--bind $(pwd -P) $(pwd -P) --chdir $(pwd -P)"

run_bin "" -y -n -C prog
assert_stdout_has "-C is short for --cwd" \
    "--bind $(pwd -P) $(pwd -P) --chdir $(pwd -P)"

run_bin "" -y -n -P "$HOME" --allow-home prog
assert_stdout_has "--allow-home permits exposing \$HOME as the project" \
    "--bind $HOME $HOME --chdir $HOME"

run_bin "" -y -n -P "$PROJ" -w "$HOME" --allow-home prog
assert_stdout_has "--allow-home permits a --rw directory equal to \$HOME" \
    "--bind $HOME $HOME"

run_bin "" -y -n -P "$HOME" --dangerous prog
assert_stdout_has "--dangerous is an alias for --allow-home" \
    "--bind $HOME $HOME --chdir $HOME"

run_bin "" -y -n -P "$HOME" prog
assert_fail "project == \$HOME is still refused without the override" \
    'refusing to expose all of $HOME'

run_bin "" -y -n -P "$PROJ" -w "$HOME" prog
assert_fail "--rw == \$HOME is still refused without the override" \
    'refusing to expose all of $HOME'

echo "== generic fallback =="

run_bin "" -y -n -P "$PROJ" no-such-program
assert_stdout_has "generic fallback works with --yes" "--unshare-all"

run_bin "" -y -n -P "$PROJ" no-such-program
assert_stderr_has "generic fallback warns about the missing profile" \
    "no profile for 'no-such-program', falling back to 'generic'"

run_bin "" -y -n -P "$PROJ" no-such-program
assert_stdout_lacks "generic profile has no network" "--share-net"

# non-TTY and no --yes: must not prompt, must fail
run_bin "" -n -P "$PROJ" no-such-program
assert_fail "non-TTY fallback without --yes is refused" "not a TTY"

run_bin "" -y -n -P "$PROJ" -p generic prog
assert_stdout_has "-p generic uses the generic profile" "--hostname bwrap-generic"

echo "== first launch bootstrap =="

FRESH="$TMP/fresh/config"
run_raw "" env XDG_CONFIG_HOME="$FRESH" HOME="$HOME" "$BIN" \
    -y -n -P "$PROJ" no-such-program
assert_stdout_has "first launch seeds generic.conf and uses it" "--unshare-all"

if [ -f "$FRESH/bubbleload/generic.conf" ]; then
    ok_test "first launch creates the config directory"
else
    fail_test "first launch creates the config directory" \
        "$FRESH/bubbleload/generic.conf does not exist"
fi

if cmp -s "$FRESH/bubbleload/generic.conf" "$EXAMPLES/generic.conf"; then
    ok_test "seeded generic.conf matches the shipped example"
else
    fail_test "seeded generic.conf matches the shipped example" \
        "differs from $EXAMPLES/generic.conf"
fi

MODE=$(stat -c %a "$FRESH/bubbleload/generic.conf" 2>/dev/null || echo "?")
if [ "$MODE" = "644" ]; then
    ok_test "seeded generic.conf has mode 0644"
else
    fail_test "seeded generic.conf has mode 0644" "mode is $MODE"
fi

printf '\n# user edit\n' >>"$FRESH/bubbleload/generic.conf"
run_raw "" env XDG_CONFIG_HOME="$FRESH" HOME="$HOME" "$BIN" \
    -y -n -P "$PROJ" no-such-program
if grep -qF 'user edit' "$FRESH/bubbleload/generic.conf"; then
    ok_test "an existing config directory is never re-seeded"
else
    fail_test "an existing config directory is never re-seeded" \
        "generic.conf was overwritten"
fi

FRESH2="$TMP/fresh2"
run_raw "" env XDG_CONFIG_HOME="$FRESH2" HOME="$HOME" "$BIN" -l
assert_stdout_has "-l seeds the config directory on first launch" "generic"

# Deleting generic.conf on purpose must keep the regeneration prompt.
KEEPCFG="$TMP/keep/config"
mkdir -p "$KEEPCFG/bubbleload"
printf 'hostname kept\n' >"$KEEPCFG/bubbleload/other.conf"
run_raw "" env XDG_CONFIG_HOME="$KEEPCFG" HOME="$HOME" "$BIN" \
    -y -n -P "$PROJ" prog
assert_fail "an existing config dir is never auto-seeded" \
    "generic profile is missing"
if [ -e "$KEEPCFG/bubbleload/generic.conf" ]; then
    fail_test "no generic.conf appears in an existing config dir" \
        "$KEEPCFG/bubbleload/generic.conf was created"
else
    ok_test "no generic.conf appears in an existing config dir"
fi

FRESH3="$TMP/fresh3"
run_raw "" env XDG_CONFIG_HOME="$FRESH3" HOME="$HOME" "$BIN" -n -P "$PROJ" prog
assert_fail "first launch still asks for --yes without a TTY" "not a TTY"

if [ "$(id -u)" -ne 0 ]; then
    RODIR="$TMP/readonly"
    mkdir -p "$RODIR"
    chmod 0555 "$RODIR"
    run_raw "" env XDG_CONFIG_HOME="$RODIR/config" HOME="$HOME" "$BIN" \
        -y -n -P "$PROJ" prog
    assert_fail "failed seeding falls back to the normal error" \
        "generic profile is missing"
    chmod 0755 "$RODIR"
fi

echo "== extra binds =="

run_bin "" -y -n -p extra-default -P "$PROJ" -w "$RW" -r "$RO" prog
assert_argv "extra binds default to just before project" -- \
    bwrap --hostname extra-default \
    --ro-bind "$RO" "$RO" --bind "$RW" "$RW" \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" prog

run_bin "" -y -n -p extra-explicit -P "$PROJ" -w "$RW" prog
assert_argv "explicit extra-binds directive controls placement" -- \
    bwrap --hostname extra-explicit --first \
    --bind "$RW" "$RW" \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" prog

run_bin "" -y -n -p no-proj -P "$PROJ" -w "$RW" prog
assert_argv "extra binds go last without a project directive" -- \
    bwrap --hostname no-proj --bind "$RW" "$RW" prog

echo "== argument passthrough =="

run_bin "" -y -n -p plain -P "$PROJ" prog -w --rw "x y" "it's" '$HOME' "" -p
assert_argv "program args survive byte for byte" -- \
    bwrap --die-with-parent \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" \
    prog -w --rw "x y" "it's" '$HOME' "" -p

run_bin "" -y -n -p exec-args -P "$PROJ" prog -s abc --rw /x
assert_argv "exec directive gets program args appended" -- \
    bwrap --hostname exec-args \
    --bind "$PROJ" "$PROJ" --chdir "$PROJ" \
    run-me --fixed -s abc --rw /x

echo "== opencode profile matches the original script =="

# Port of the bwrap-opencode shell script: build the expected argument vector
# by hand so the profile translation is compared word for word.
reference_opencode() {
    local project=$1
    local rw=$2
    local ro=$3
    local resolv

    shift 3
    REF=()
    REF+=(bwrap --die-with-parent --unshare-all --share-net)
    REF+=(--hostname bwrap-opencode)
    REF+=(--ro-bind /usr /usr)
    REF+=(--symlink usr/bin /bin --symlink usr/bin /sbin)
    REF+=(--symlink usr/lib /lib --symlink usr/lib /lib64)
    REF+=(--ro-bind /etc /etc)
    resolv=$(realpath -e /etc/resolv.conf 2>/dev/null) || resolv=""
    case "$resolv" in
        "" | /etc/*) ;;
        *) REF+=(--ro-bind "$resolv" "$resolv") ;;
    esac
    REF+=(--proc /proc --dev /dev --tmpfs /tmp)
    REF+=(--dir /tmp/opencode)
    REF+=(--bind /tmp/opencode /tmp/opencode)
    REF+=(--tmpfs "$HOME")
    REF+=(--ro-bind "$HOME/.config/zsh" "$HOME/.config/zsh")
    REF+=(--bind "$HOME/.config/opencode" "$HOME/.config/opencode")
    REF+=(--bind "$HOME/.local/share/opencode" "$HOME/.local/share/opencode")
    REF+=(--bind "$HOME/.local/state/opencode" "$HOME/.local/state/opencode")
    REF+=(--bind "$HOME/.cache/opencode" "$HOME/.cache/opencode")
    REF+=(--ro-bind-try "$HOME/.config/git" "$HOME/.config/git")
    REF+=(--ro-bind /dev/null "$HOME/.config/git/credentials")
    REF+=(--ro-bind "$ro" "$ro")
    REF+=(--bind "$rw" "$rw")
    REF+=(--bind "$project" "$project")
    REF+=(--chdir "$project")
    REF+=(--setenv HOME "$HOME")
    REF+=(--bind-try "$HOME/.cache/npm" "$HOME/.cache/npm")
    REF+=(--bind-try "$HOME/.local/share/npm" "$HOME/.local/share/npm")
    REF+=(--bind-try "$HOME/.config/npm" "$HOME/.config/npm")
    REF+=(--bind-try "$HOME/.local/bin" "$HOME/.local/bin")
    REF+=(--setenv npm_config_cache "$HOME/.cache/npm")
    REF+=(--unsetenv DISPLAY)
    REF+=(--unsetenv WAYLAND_DISPLAY)
    REF+=(--unsetenv WAYLAND_SOCKET)
    REF+=(--unsetenv XAUTHORITY)
    REF+=(opencode --standalone "$@")
}

reference_opencode "$PROJ" "$RW" "$RO" -s abc
run_bin "" -n -p opencode -P "$PROJ" -w "$RW" -r "$RO" opencode -s abc
assert_argv "opencode.conf == original script argument list" -- "${REF[@]}"

echo "== completions =="

COMPDIR="$TESTDIR/../completions"
COMPFIX="$TMP/compfix"
mkdir -p "$COMPFIX/adir"
: >"$COMPFIX/alpha"
: >"$COMPFIX/album"

comp_bash() { # cword words...
    bash -c '
        XDG_CONFIG_HOME=$1
        export XDG_CONFIG_HOME
        source "$2"
        COMP_CWORD=$3
        shift 3
        COMP_WORDS=("$@")
        _bubbleload
        printf "%s\n" "${COMPREPLY[@]}"
    ' _ "$XDG_CONFIG_HOME" "$COMPDIR/bubbleload.bash" "$@"
}

comp_posix() { # COMP_LINE
    COMP_LINE=$1 sh "$COMPDIR/bubbleload.sh"
}

if bash -n "$COMPDIR/bubbleload.bash" 2>/dev/null; then
    ok_test "bash completion parses"
else
    fail_test "bash completion parses" "$(bash -n "$COMPDIR/bubbleload.bash" 2>&1)"
fi

out=$(comp_bash 2 bubbleload -p "")
if grep -qx 'inner' <<<"$out"; then
    ok_test "bash completion: -p completes profiles"
else
    fail_test "bash completion: -p completes profiles" "got: $out"
fi

out=$(comp_bash 1 bubbleload --r)
if [ "$out" = "$(printf '%s\n' --rw --ro)" ]; then
    ok_test "bash completion: options before BINARY"
else
    fail_test "bash completion: options before BINARY" "got: $out"
fi

out=$(comp_bash 2 bubbleload prog -)
if [ -z "$out" ]; then
    ok_test "bash completion: no loader options after BINARY"
else
    fail_test "bash completion: no loader options after BINARY" "got: $out"
fi

out=$(comp_posix 'bubbleload -p ')
if grep -qx 'inner' <<<"$out"; then
    ok_test "posix completion: -p completes profiles"
else
    fail_test "posix completion: -p completes profiles" "got: $out"
fi

out=$(comp_posix 'bubbleload --r')
if [ "$out" = "$(printf '%s\n' --ro --rw)" ]; then
    ok_test "posix completion: options"
else
    fail_test "posix completion: options" "got: $out"
fi

out=$(comp_posix "bubbleload prog $COMPFIX/al")
if [ "$out" = "$(printf '%s\n' "$COMPFIX/album" "$COMPFIX/alpha")" ]; then
    ok_test "posix completion: files after BINARY"
else
    fail_test "posix completion: files after BINARY" "got: $out"
fi

out=$(comp_posix "bubbleload -P \"$COMPFIX/a")
if [ "$out" = "$COMPFIX/adir/" ]; then
    ok_test "posix completion: quoted directory only yields dirs"
else
    fail_test "posix completion: quoted directory only yields dirs" "got: $out"
fi

out=$(comp_posix "bubbleload prog -w $COMPFIX/")
if [ "$out" = "$(printf '%s\n' "$COMPFIX/adir" "$COMPFIX/album" \
    "$COMPFIX/alpha")" ]; then
    ok_test "posix completion: -w after BINARY is a program argument"
else
    fail_test "posix completion: -w after BINARY is a program argument" \
        "got: $out"
fi

out=$(comp_posix 'bubbleload -- ')
if grep -qx 'sh' <<<"$out"; then
    ok_test "posix completion: commands after --"
else
    fail_test "posix completion: commands after --" "got: $out"
fi

if command -v zsh >/dev/null 2>&1; then
    if zsh -n "$COMPDIR/_bubbleload" 2>/dev/null; then
        ok_test "zsh completion parses"
    else
        fail_test "zsh completion parses" "$(zsh -n "$COMPDIR/_bubbleload" 2>&1)"
    fi
else
    echo "skip zsh completion: zsh is not installed"
fi

if command -v fish >/dev/null 2>&1; then
    if fish --no-execute "$COMPDIR/bubbleload.fish" 2>/dev/null; then
        ok_test "fish completion parses"
    else
        fail_test "fish completion parses" \
            "$(fish --no-execute "$COMPDIR/bubbleload.fish" 2>&1)"
    fi
    out=$(fish -c "source $COMPDIR/bubbleload.fish; complete -C 'bubbleload -p '" 2>/dev/null)
    if grep -qx 'inner' <<<"$out"; then
        ok_test "fish completion: -p completes profiles"
    else
        fail_test "fish completion: -p completes profiles" "got: $out"
    fi
else
    echo "skip fish completion: fish is not installed"
fi

echo "== misc =="

run_bin "" -l
assert_stdout_has "-l lists profiles" "opencode"

run_bin "" -l
if [ "$RC" -ne 0 ]; then
    fail_test "-l output is sorted" "exit $RC"
else
    sorted=$(printf '%s\n' "$OUT" | sort)
    if [ "$sorted" != "$OUT" ]; then
        fail_test "-l output is sorted" "got: $OUT"
    else
        ok_test "-l output is sorted"
    fi
fi

if [ "${BUBBLELOAD_SKIP_SMOKE:-0}" != "0" ]; then
    echo "skip smoke test: BUBBLELOAD_SKIP_SMOKE is set"
elif command -v bwrap >/dev/null 2>&1; then
    run_bin "" -y -p generic -P "$PROJ" sh -c 'echo ok'
    if [ "$RC" -ne 0 ]; then
        fail_test "smoke test: real sandbox runs a command" \
            "exit $RC" "stderr: $ERR"
    elif [ "$OUT" != "ok" ]; then
        fail_test "smoke test: real sandbox runs a command" \
            "unexpected stdout: $OUT"
    else
        ok_test "smoke test: real sandbox runs a command"
    fi
else
    echo "skip smoke test: bwrap is not installed"
fi

echo
printf '%d passed, %d failed\n' "$passes" "$failures"
[ "$failures" -eq 0 ]
