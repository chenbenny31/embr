#!/bin/bash
# checkbench.sh <file> <runs> <label>=<embr binary>[,ENV=value...] ...
# How long a pull takes to find out that it has nothing, or one chunk, to fetch:
#   complete  the output already is the file                       -> every chunk hashed, nothing requested
#   resume    a partial download with every chunk but the last one -> those chunks hashed, one requested
# PULL_CPUS=list pins the pull. TCP on loopback, both files in memory (tmpfs). Per case: the pull's wall time and its user+sys, median of the runs.
set -uo pipefail
S=${SCRATCH:?scratch directory}
file=$1 runs=$2; shift 2
work=$S/resume/run; mkdir -p "$work"; out=$work/out.bin
port=10041
size=$(stat -c %s "$file"); chunks=$(( (size + 1048575) / 1048576 ))
echo "# $(basename "$file"): $size bytes, $chunks chunks; kernel $(uname -r); $(nproc) CPUs; pull on CPUs ${PULL_CPUS:-all}; n=$runs; $(date -u +%Y-%m-%dT%H:%M:%SZ)"
cp "$file" "$out"
partial=$work/.out.bin.embr.partial
make_partial() { # every chunk recorded as received but the last
    python3 - "$file" "$partial" "$chunks" <<'PY'
import hashlib, sys
path, partial, chunks = sys.argv[1], sys.argv[2], int(sys.argv[3])
with open(path, 'rb') as f: first = f.read(1048576)
bitmap = bytearray((chunks + 7) // 8)
for i in range(chunks - 1): bitmap[i // 8] |= 1 << (i % 8)
with open(partial, 'wb') as f: f.write(hashlib.sha256(first).digest() + bytes(bitmap))
PY
}
one() { # <case> <binary> <env...> -> "wall user+sys line-of-the-pull"
    local kind=$1 embr=$2; shift 2
    ( HOME=$S/home126 "$embr" push "$file" --port $port > "$work/push.log" 2>&1 & )
    local tries=0
    until [[ -n "$(ss -H -lnt "sport = :$port" 2>/dev/null)" ]]; do
        sleep 0.05; tries=$((tries + 1)); (( tries == 1200 )) && { echo "the push never listened: $(tail -1 "$work/push.log")" >&2; exit 1; }
    done
    rm -f "$partial"; [[ $kind == resume ]] && make_partial
    HOME=$S/home126 ${PULL_CPUS:+taskset -c $PULL_CPUS} env "$@" /usr/bin/time -f "%e %U %S" -o "$work/pull.time" "$embr" pull 127.0.0.1 --port $port --out "$out" > "$work/pull.log" 2>&1
    local waited=0
    while pgrep -f "[e]mbr push .*--port $port" > /dev/null; do sleep 0.05; waited=$((waited + 1)); (( waited == 60 )) && pkill -f "[e]mbr push .*--port $port"; done
    echo "$(awk '{ print $1, $2 + $3 }' "$work/pull.time") $(grep -ao 'already complete\|resuming partial download - [0-9]* chunks verified on disk\|requesting [0-9/]* chunks' "$work/pull.log" | tr '\n' ';')"
}
first=${1#*=}; one complete "${first%%,*}" > /dev/null # the sender hashes the file once and keeps the hashes beside it
for kind in complete resume; do
    for spec in "$@"; do
        label=${spec%%=*}; rest=${spec#*=}; embr=${rest%%,*}; envs=$(echo "${rest#"$embr"}" | tr ',' ' ')
        : > "$work/res"
        # shellcheck disable=SC2086
        for (( r = 0; r < runs; r++ )); do one $kind "$embr" $envs >> "$work/res"; done
        awk -v kind="$kind" -v label="$label" '{ w[NR] = $1; c[NR] = $2; msg = $0; sub(/^[^ ]+ [^ ]+ /, "", msg) }
            function med(a,   i, j, t, b) { for (i = 1; i <= NR; i++) b[i] = a[i]; for (i = 1; i <= NR; i++) for (j = i + 1; j <= NR; j++) if (b[j] < b[i]) { t = b[i]; b[i] = b[j]; b[j] = t } return b[int((NR + 1) / 2)] }
            END { printf "%-9s %-22s wall=%.2fs cpu=%.2fs  %s\n", kind, label, med(w), med(c), msg }' "$work/res"
    done
done
cmp -s "$file" "$out" && echo "# output intact after the last run" || echo "# OUTPUT DIFFERS"
rm -f "$out" "$partial"
