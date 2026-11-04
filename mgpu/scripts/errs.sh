#!/bin/sh
# Summarises MSVC errors from a build output file: unique lines, then counts per file.
grep -E ": (fatal )?error" "$1" | sed -E 's/ \[[^]]*\]$//' | sort -u > "$1.err"
wc -l < "$1.err"
sed -E 's/\([0-9,]+\).*//' "$1.err" | sort | uniq -c
