#!/bin/sh
#
# bubbleload completion command for POSIX sh.
#
# POSIX sh has no standard programmable completion API: dash, ksh and mksh
# cannot run a completion function the way bash, zsh and fish can.  What every
# POSIX shell *can* do is run an external command for TAB, so this script
# implements that protocol: it reads the command line being completed and
# prints one candidate per line.
#
# bash can use it directly:
#
#     complete -C /usr/share/bubbleload/completions/bubbleload.sh bubbleload
#
# and any other shell with an "external completion command" hook can point at
# it the same way.  It understands $COMP_LINE and $COMP_POINT (both set by
# bash), so it works when completing in the middle of a line too:
#
#     COMP_LINE='bubbleload -p ' COMP_POINT=15 ./bubbleload.sh
#
# The script never modifies anything and always exits 0, so a failure can not
# break the calling shell.

set -u

line=${COMP_LINE:-}
point=${COMP_POINT:-}

if [ -z "$line" ]; then
    exit 0
fi

# Cut the line at the cursor position.  awk gives character offsets (POSIX)
# instead of the byte offsets a naive ${line%...} would need.
if [ -n "$point" ]; then
    line=$(printf '%s\n' "$line" | awk -v p="$point" 'NR == 1 { print substr($0, 1, p) }')
fi

# awk splits the line into shell words (quotes and backslashes included) and
# decides what the word under the cursor is.  Only the decision is returned:
# the shell below enumerates files from the file system.
decision=$(
    printf '%s\n' "$line" | awk '
    # Character constants: comparing a one-character string with a number (34,
    # 39, 92) would coerce it to 0 instead of comparing the character.
    BEGIN { sq = sprintf("%c", 39); dq = sprintf("%c", 34); bs = sprintf("%c", 92) }
    function is_value_opt(w) {
        return w == "-p" || w == "--profile" || w == "-w" || w == "--rw" ||
               w == "-r" || w == "--ro" || w == "-P" || w == "--project"
    }
    function opt_mode(w) {
        if (w == "-p" || w == "--profile") return "profiles"
        return "dirs"
    }
    {
        n = 0
        word = ""
        started = 0
        quote = 0
        len = length($0)

        for (i = 1; i <= len; i++) {
            c = substr($0, i, 1)
            if (quote == sq) {
                started = 1
                if (c == sq) quote = 0
                else word = word c
                continue
            }
            if (quote == dq) {
                started = 1
                if (c == dq) quote = 0
                else if (c == bs && i < len) { i++; word = word substr($0, i, 1) }
                else word = word c
                continue
            }
            if (c == " " || c == "\t") {
                if (started) { words[++n] = word; word = ""; started = 0 }
                continue
            }
            if (c == sq) { quote = sq; started = 1; continue }
            if (c == dq) { quote = dq; started = 1; continue }
            if (c == bs && i < len) {
                i++
                word = word substr($0, i, 1)
                started = 1
                continue
            }
            word = word c
            started = 1
        }

        if (started) { cur = word; ncomp = n }
        else { cur = ""; ncomp = n }

        if (ncomp < 1) {
            print "none"
            print cur
            exit
        }

        phase = "opt"
        want = ""
        prog_seen = 0
        for (j = 2; j <= ncomp; j++) {
            w = words[j]
            if (phase == "prog") {
                prog_seen = 1
                continue
            }
            if (want != "") { want = ""; continue }
            if (w == "--") { phase = "prog"; continue }
            if (is_value_opt(w)) { want = w; continue }
            if (substr(w, 1, 1) == "-" && w != "-") continue
            phase = "prog"
            prog_seen = 1
        }

        if (phase == "prog") {
            print (prog_seen ? "files" : "commands")
        } else if (want != "") {
            print opt_mode(want)
        } else if (substr(cur, 1, 1) == "-") {
            print "options"
        } else {
            print "commands"
        }
        print cur
    }
    '
)

mode=$(printf '%s\n' "$decision" | head -n 1)
cur=$(printf '%s\n' "$decision" | tail -n +2)

{
    case $mode in
    options)
        for o in -h --help -l --list -y --yes -n --dry-run \
            -p --profile -w --rw -r --ro -P --project -C --cwd \
            --allow-home --dangerous; do
            case $o in
            "$cur"*) printf '%s\n' "$o" ;;
            esac
        done
        ;;
    profiles)
        dir="${XDG_CONFIG_HOME:-$HOME/.config}/bubbleload"
        for f in "$dir"/*.conf; do
            [ -f "$f" ] || continue
            name=${f##*/}
            name=${name%.conf}
            case $name in
            "$cur"*) printf '%s\n' "$name" ;;
            esac
        done
        ;;
    dirs | files)
        case $cur in
        */*) gdir=${cur%/*}; pfx=$gdir/; base=${cur##*/} ;;
        *) gdir=.; pfx=; base=$cur ;;
        esac
        [ -n "$gdir" ] || gdir=/
        case $base in
        .*) show_hidden=1 ;;
        *) show_hidden=0 ;;
        esac
        for f in "$gdir"/* "$gdir"/.*; do
            [ -e "$f" ] || [ -L "$f" ] || continue
            name=${f##*/}
            case $name in
            . | ..) continue ;;
            esac
            if [ "$show_hidden" = 0 ]; then
                case $name in
                .*) continue ;;
                esac
            fi
            case $name in
            "$base"*) ;;
            *) continue ;;
            esac
            if [ "$mode" = dirs ]; then
                [ -d "$f" ] || continue
                printf '%s%s/\n' "$pfx" "$name"
            else
                printf '%s%s\n' "$pfx" "$name"
            fi
        done
        ;;
    commands)
        old_ifs=$IFS
        IFS=:
        for d in ${PATH:-/usr/local/bin:/usr/bin:/bin}; do
            IFS=$old_ifs
            [ -n "$d" ] || d=.
            for f in "$d"/*; do
                [ -f "$f" ] && [ -x "$f" ] || continue
                name=${f##*/}
                case $name in
                "$cur"*) printf '%s\n' "$name" ;;
                esac
            done
            IFS=:
        done
        IFS=$old_ifs
        ;;
    esac
} | sort -u

exit 0
