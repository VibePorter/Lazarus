#!/bin/bash
# run.sh <script> <log>: runs a tools/nx script in WSL with a log, prints a summary.
S=$1; L=${2:-/root/run.log}
bash "/mnt/c/ME2_Recomp/tools/nx/$S" > "$L" 2>&1; rc=$?
echo "EXIT=$rc" >> "$L"
grep -E '^==|error|EXIT' "$L" | tail -20; echo ---; tail -8 "$L"
