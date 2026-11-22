#!/bin/bash
# Follow-up experiments on the two hosts once the four passes are done.
#   extras.sh pass <name> '<rows|receiver env|sender env>' [RUNS] [WARMUP]   one more wan.sh pass, its own run directory
#   extras.sh profile <row> [receiver env]     system-wide flat perf profile on both hosts over 1 warmup + 3 runs of one row
#   extras.sh streams <count>                  <count> nc transfers at once: is the path ceiling per flow or per instance?
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
get()  { sed -n "s/^$1=//p" "$here/state" | tail -1; }
SSH_OPTS=(-i "$here/desktop-key" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile="$here/known_hosts"
          -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes -o ServerAliveInterval=15)
on()   { local role=$1; shift; ssh "${SSH_OPTS[@]}" "ec2-user@$(get "${role}_ip")" "$@"; }
wan()  { on recv "cd embr && SENDER_IP=$(get send_ip) SSH_KEY=\$HOME/.ssh/bench $* bash bench/wan.sh"; }
hide() { sed -e "s/$(get send_ip)/SENDER/g" -e "s/$(get recv_ip)/RECEIVER/g"; }

cmd_pass() { # the run directory on the receiver gets a name file so the fetch can tell the passes apart
    local name=$1 spec=$2 runs=${3:-10} warmup=${4:-2}
    wan "PASSES=custom CUSTOM='$spec' RUNS=$runs WARMUP=$warmup EXTRA_NAME=$name" 2>&1 | hide
    on recv "d=\$(ls -dt ~/embr-wan/2*/ | head -1); echo '$name' > \$d/extra-name.txt; echo \"\$d -> $name\""
}

cmd_profile() { # records on both hosts and keeps ~/perf/<tag>.data there; cmd_report writes the text
    local row=$1 recv_env=${2:-} role tag
    tag=$row${recv_env:+.$(tr -c 'A-Za-z0-9\n' '-' <<< "$recv_env" | sed 's/-*$//')}
    for role in send recv; do
        on "$role" "command -v perf > /dev/null || sudo dnf install -y -q perf > /dev/null 2>&1; mkdir -p ~/perf; sudo rm -f ~/perf/$tag.data
            ( sudo nohup setsid perf record -a -e cpu-clock -F 499 -o /home/ec2-user/perf/$tag.data > /tmp/perf.log 2>&1 < /dev/null & ); sleep 1; pgrep -x perf > /dev/null && echo '$role: perf recording'"
    done
    wan "PASSES=custom CUSTOM='$row|$recv_env|' RUNS=3 WARMUP=1" 2>&1 | hide | grep -E '^\[wan\] exit|FATAL|FAILED' | cut -c1-200
    for role in send recv; do on "$role" 'sudo pkill -INT -x perf; while pgrep -x perf > /dev/null; do sleep 0.3; done'; done
    on recv "d=\$(ls -dt ~/embr-wan/2*/ | head -1); echo 'profile-$tag' > \$d/extra-name.txt"
    cmd_report "$tag"
}

cmd_report() { # <tag>: percentages are of all samples in every table, so the tables add up
    local tag=$1 role side
    mkdir -p "$here/profiles"
    for role in send recv; do
        side=$( [[ $role == send ]] && echo sender || echo receiver )
        on "$role" 'data=/home/ec2-user/perf/'"$tag"'.data
            report() { sudo perf report -i $data --stdio --no-children "$@" 2> /dev/null | grep -v -e "^$" -e "^# *$" -e "^# \(Total Lost\|Overhead\|\.\.\.\|To display\|(Tip\)" | sed "s/ *$//" | cut -c1-150; }
            echo "# system-wide cpu-clock profile at 499 Hz, flat, over one wan.sh pass of 1 warmup + 3 runs of one row ('"$tag"')"
            echo "# $(uname -r), $(nproc) CPUs, $(date -u +%FT%TZ). The whole pass is sampled, so idle time, the gaps between"
            echo "# transfers and the script checks (sha256sum, openssl speed) are in it. Every percentage is of all samples:"
            echo "# divide by the sum of the embr lines to get shares of embr."
            echo; echo "## every sample, by command and object"
            report --sort comm,dso --percent-limit 0.03
            echo; echo "## embr, by symbol"
            report --comms embr --sort dso,symbol --percent-limit 0.04 | grep -v "^# \(Samples\|Event\)"
            echo; echo "## interrupt-side work outside embr: swapper and ksoftirqd, idle loop left out"
            report --comms swapper,ksoftirqd/0,ksoftirqd/1 --sort comm,symbol --percent-limit 0.04 | grep -v -e "^# \(Samples\|Event\)" -e halt -e default_idle | sed "s/ *- *- *$//"' > "$here/profiles/profile.$tag.$side.txt"
        echo "$side: $(wc -l < "$here/profiles/profile.$tag.$side.txt") lines"
    done
}

cmd_streams() { # ncat on ports 9999 and 10007.. (the open TCP ports); every stream sends the same 1 GiB file
    local count=$1 ports=(9999 10007) port i
    (( count <= ${#ports[@]} )) || { echo "only ${#ports[@]} TCP ports are open" >&2; exit 1; }
    for (( i = 0; i < count; i++ )); do
        port=${ports[$i]}
        on send "( nohup setsid ncat -l $port --send-only < /tmp/bench_wan.bin > /dev/null 2>&1 & )"
    done
    sleep 1
    on recv "sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
        start=\$(date +%s.%N)
        for port in ${ports[*]:0:$count}; do ( s=\$(date +%s.%N); bytes=\$(ncat --recv-only $(get send_ip) \$port | wc -c); e=\$(date +%s.%N); echo \"stream port=\$port bytes=\$bytes seconds=\$(echo \"\$e - \$s\" | bc -l | cut -c1-6)\" ) & done
        wait; end=\$(date +%s.%N)
        echo \"streams=$count wall=\$(echo \"\$end - \$start\" | bc -l | cut -c1-6) total_gbps=\$(echo \"$count * 1073741824 * 8 / (\$end - \$start) / 1000000000\" | bc -l | cut -c1-5)\"" | hide
}

cmd=${1:-}; shift || true
case "$cmd" in
    pass|profile|report|streams) "cmd_$cmd" "$@" ;;
    *) sed -n '2,5p' "$0"; exit 2 ;;
esac
