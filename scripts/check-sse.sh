#!/usr/bin/env bash
# check-sse.sh <rootfs.tar | directory>
#
# Safety net for the Geode LX (i586 + MMX/3DNow/CMOV, no SSE/SSE2): scans every
# 32-bit ELF in a rootfs for instructions that would raise SIGILL.
#
#   SSE   any xmm/ymm/zmm operand, cvt*, ldmxcsr/stmxcsr, MMX-extension ops
#         (pshufw, pmaxub, ...), sfence/lfence/mfence, movnt*, clflush.
#   NOPL  multi-byte NOP (0F 1F: nopw/nopl) -- reported separately as a warning.
#
# Many libraries carry SSE paths guarded by a runtime CPUID check, so each file
# is classified, not just counted:
#   ERROR   SSE in a startup symbol (_start, _init, main, __libc_start_main...),
#           or in more than 5 functions whose names do NOT suggest runtime
#           dispatch (sse/avx/simd/xmm/ifunc/multiarch/cpuid/aesni/pclmul/fma).
#           Stripped code without symbols counts 1 "function" per 10 instructions.
#   WARN    SSE confined to <=5 non-dispatch functions ("review"), or only in
#           dispatch-named functions (any number: "dispatch").
# Exit status is non-zero only if at least one ERROR exists.
#
# Env: JOBS=<n> parallelism (default: nproc), VERBOSE=1 also lists clean files count.
set -euo pipefail

usage() { echo "usage: $0 <rootfs.tar | directory>" >&2; exit 2; }
[ $# -eq 1 ] || usage
SRC=$1
[ -e "$SRC" ] || { echo "check-sse: no such file or directory: $SRC" >&2; exit 2; }
command -v objdump >/dev/null || { echo "check-sse: objdump not found" >&2; exit 2; }

JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/check-sse.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- Resolve the tree to scan ----------------------------------------------
if [ -d "$SRC" ]; then
    ROOT=$SRC
else
    ROOT=$WORK/root
    mkdir -p "$ROOT"
    echo "check-sse: extracting $SRC ..." >&2
    # Device nodes need root and are irrelevant here; permissions fixed below.
    tar -xf "$SRC" -C "$ROOT" --no-same-owner --exclude='./dev/*' --exclude='dev/*' 2>/dev/null || {
        echo "check-sse: tar extraction failed" >&2; exit 2; }
    chmod -R u+rX "$ROOT" 2>/dev/null || true
fi

# --- Per-file worker: prints one TSV record for ELF32 files with findings ---
# Record: CLASS \t file \t sse_insns \t sse_funcs \t nop_insns \t nop_funcs \t detail
scan_one() {
    local f=$1 magic
    # Builtin read of 5 bytes: "\x7fELF\x01" = 32-bit ELF (no fork per file).
    { IFS= read -r -N 5 magic; } <"$f" 2>/dev/null || return 0
    [ "$magic" = $'\177ELF\001' ] || return 0
    objdump -d -w --no-show-raw-insn "$f" 2>/dev/null | awk -v F="${f#"$ROOT"}" '
    function mnemonic(s,   n, a, i) {
        n = split(s, a, /[ \t]+/)
        for (i = 1; i <= n; i++) {
            if (a[i] == "" ) continue
            if (a[i] ~ /^(rep|repz|repnz|repe|repne|lock|data16|addr16|cs|ds|es|ss|fs|gs|bnd|notrack|xacquire|xrelease)$/) continue
            return a[i]
        }
        return ""
    }
    BEGIN {
        fn = "-"
        core = "^(_start|_init|_fini|main|__libc_start_main|__libc_start_main_impl|_dl_start|__dl_start|_dl_relocate_static_pie)$"
        disp = "(sse|avx|simd|xmm|ifunc|multiarch|cpuid|aesni|pclmul|fma)"
        ssemn = "^(cvt|ldmxcsr|stmxcsr|movnt|pshufw|pmaxub|pminub|pmaxsw|pminsw|pavgb|pavgw|pmulhuw|psadbw|pextrw|pinsrw|pmovmskb|maskmovq|movmskp|sfence|lfence|mfence|clflush)"
    }
    /^[0-9a-f]+ <.*>:$/ { fn = $0; sub(/^[0-9a-f]+ </, "", fn); sub(/>:$/, "", fn); next }
    /^ +[0-9a-f]+:\t/ {
        ins = $0; sub(/^ +[0-9a-f]+:\t/, "", ins)
        # cheap pre-filter first: awk is the bottleneck on big binaries
        if (ins !~ /mm[0-9]|cvt|mxcsr|movnt|pshufw|pmaxub|pminub|pmaxsw|pminsw|pavg|pmulhuw|psadbw|pextrw|pinsrw|pmovmskb|maskmovq|movmskp|fence|clflush|nop[wl]/) next
        mn = mnemonic(ins)
        if (ins ~ /%[xyz]mm[0-9]/ || mn ~ ssemn) { ns++; sf[fn]++ }
        else if (mn ~ /^nop[wl]$/)            { nn++; nf[fn]++ }
    }
    END {
        if (ns == 0 && nn == 0) exit
        cls = "CLEAN"; detail = ""
        if (ns > 0) {
            nd = 0; nfun = 0; corehit = 0; list = ""
            for (k in sf) {
                nfun++
                if (k ~ core) corehit = 1
                if (k == "-") nd += int(sf[k] / 10) + 1
                else if (k !~ disp) nd++
                if (nfun <= 6) list = list (list == "" ? "" : ",") k "(" sf[k] ")"
            }
            if (nfun > 6) list = list ",..."
            if (corehit || nd > 5) cls = "ERROR"
            else if (nd > 0) cls = "WARN-review"
            else cls = "WARN-dispatch"
            detail = list
        } else cls = "WARN-nopl"
        nfn = 0; for (k in nf) nfn++
        printf "%s\t%s\t%d\t%d\t%d\t%d\t%s\n", cls, F, ns + 0, nfun + 0, nn + 0, nfn, detail
    }'
}
export -f scan_one
export ROOT

# --- Scan -------------------------------------------------------------------
echo "check-sse: scanning $ROOT with $JOBS jobs ..." >&2
find "$ROOT" -xdev \( -path "$ROOT/proc" -o -path "$ROOT/sys" -o -path "$ROOT/dev" \) -prune -o -type f -size +0 -print0 \
    | xargs -0 -r -P "$JOBS" -n 16 bash -c 'for f in "$@"; do scan_one "$f"; done' _ \
    | LC_ALL=C sort > "$WORK/results.tsv"
NFILES=$(find "$ROOT" -xdev \( -path "$ROOT/proc" -o -path "$ROOT/sys" -o -path "$ROOT/dev" \) -prune -o -type f -size +0 -print | wc -l)

# --- Allowlist: downgrade reviewed ERROR files (scripts/sse-allow.txt) to ALLOWED -----
ALLOW=${SSE_ALLOW:-$(dirname "$0")/sse-allow.txt}
if [ -f "$ALLOW" ] && grep -q '^ERROR' "$WORK/results.tsv"; then
    : > "$WORK/results2.tsv"
    while IFS=$'\t' read -r cls f rest; do
        if [ "$cls" = ERROR ]; then
            while read -r pat _; do
                case $pat in ''|\#*) continue ;; esac
                # shellcheck disable=SC2254
                case $f in $pat) cls=ALLOWED; break ;; esac
            done < "$ALLOW"
        fi
        printf '%s\t%s\t%s\n' "$cls" "$f" "$rest" >> "$WORK/results2.tsv"
    done < "$WORK/results.tsv"
    LC_ALL=C sort "$WORK/results2.tsv" > "$WORK/results.tsv"
fi

# --- Report ------------------------------------------------------------------
n_err=$(grep -c '^ERROR'          "$WORK/results.tsv" || true)
n_alw=$(grep -c '^ALLOWED' "$WORK/results.tsv" || true)
n_rev=$(grep -c '^WARN-review'    "$WORK/results.tsv" || true)
n_dis=$(grep -c '^WARN-dispatch'  "$WORK/results.tsv" || true)
n_nop=$(grep -c '^WARN-nopl'      "$WORK/results.tsv" || true)
n_nopany=$(awk -F'\t' '$5>0' "$WORK/results.tsv" | wc -l)

print_class() { # <regex> <title>
    local rows
    rows=$(awk -F'\t' -v c="$1" '$1==c' "$WORK/results.tsv")
    [ -n "$rows" ] || return 0
    echo "== $2"
    echo "$rows" | awk -F'\t' '{ printf "  %-60s sse=%-5d in %-3d funcs | nopl=%-5d  %s\n", $2, $3, $4, $5, $7 }'
}
print_class 'ERROR'         'ERROR: SSE in startup code or many non-dispatch functions (would SIGILL on Geode LX)'
print_class 'ALLOWED'       'ALLOWED: SSE present but listed in sse-allow.txt (runtime-guarded, reviewed)'
print_class 'WARN-review'   'WARN (review): SSE in <=5 functions without dispatch-looking names'
print_class 'WARN-dispatch' 'WARN (dispatch): SSE only in sse/avx/ifunc-named functions (probably CPUID-guarded)'
print_class 'WARN-nopl'     'WARN (nopl): only multi-byte NOP (0F 1F) found'
if [ "$n_nopany" -gt 0 ]; then
    echo "== NOPL (nopw/nopl, 0F 1F) present in $n_nopany file(s); per-file counts above or: "
    awk -F'\t' '$5>0 && $1!="WARN-nopl" { printf "  %-60s nopl=%d in %d funcs\n", $2, $5, $6 }' "$WORK/results.tsv" | head -40
fi
echo "--"
echo "check-sse: $NFILES files, ERROR=$n_err ALLOWED=$n_alw WARN-review=$n_rev WARN-dispatch=$n_dis WARN-nopl-only=$n_nop (files with any nopl: $n_nopany)"
if [ "$n_err" -gt 0 ]; then
    echo "check-sse: FAIL ($n_err file(s) with SSE in unconditional code)" >&2
    exit 1
fi
echo "check-sse: OK (no unconditional SSE found)"
