#!/bin/sh
# Embed a text file into a C string literal on stdout.
#
# Used by the Makefile to generate the built-in generic profile from
# examples/generic.conf, so the shipped example and the built-in default can
# never drift apart.
#
# usage: embed.sh FILE [VARNAME]
set -eu

file=${1:?usage: embed.sh FILE [VARNAME]}
name=${2:-bl_default_generic}

LC_ALL=C awk -v name="$name" -v file="$file" '
BEGIN {
    printf "/* Generated from %s by tools/embed.sh -- do not edit. */\n", file
    printf "static const char %s[] =\n", name
}
{
    out = ""
    n = length($0)
    for (i = 1; i <= n; i++) {
        c = substr($0, i, 1)
        if (c == "\\") {
            out = out "\\\\"
        } else if (c == "\"") {
            out = out "\\\""
        } else {
            out = out c
        }
    }
    printf "\t\"%s\\n\"\n", out
}
END {
    printf "\t;\n"
}
' "$file"
