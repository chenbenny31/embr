#!/bin/bash
# pair.sh <quic|tcp> <count>: <count> embr transfers of the same 1 GiB file at once, each its own
# push/pull pair on its own port; prints one line per pull and per push, and the total rate.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
get()  { sed -n "s/^$1=//p" "$here/state" | tail -1; }
SSH_OPTS=(-i "$here/desktop-key" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile="$here/known_hosts"
          -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes -o ServerAliveInterval=15)
on()   { local role=$1; shift; ssh "${SSH_OPTS[@]}" "ec2-user@$(get "${role}_ip")" "$@"; }
hide() { sed -e "s/$(get send_ip)/SENDER/g" -e "s/$(get recv_ip)/RECEIVER/g"; }

transport=$1 count=$2
if [[ $transport == quic ]]; then ports=(10010 10011); proto=u
    push_tail='--transport quic --cert $HOME/.config/embr/cert.pem --key $HOME/.config/embr/key.pem'
    push_env='EMBR_QUIC_GSO=1 EMBR_QUIC_EGRESS=sendmsg EMBR_QUIC_STATS=1'
else ports=(10007 9999); proto=t; push_tail='--transport tcp'; push_env=''
fi
ports=("${ports[@]:0:$count}")

on send "cd embr; rm -f /tmp/pair_push_*
    for port in ${ports[*]}; do
        ( nohup setsid /usr/bin/time -f \"push port=\$port user=%U sys=%S exit=%x\" -o /tmp/pair_push_\$port.txt env $push_env ./build/embr push /tmp/bench_wan.bin --port \$port $push_tail > /dev/null 2> /tmp/pair_push_\$port.log < /dev/null & )
    done
    for port in ${ports[*]}; do until [[ -n \"\$(ss -H -ln$proto \"sport = :\$port\")\" ]]; do sleep 0.2; done; done"
on recv "cd embr; rm -f /tmp/bench_wan_out.bin /tmp/pair_*; sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    start=\$(date +%s.%N)
    for port in ${ports[*]}; do
        ( /usr/bin/time -f \"pull port=\$port wall=%e user=%U sys=%S exit=%x\" -o /tmp/pair_pull_\$port.txt env EMBR_QUIC_STATS=1 ./build/embr pull $(get send_ip) --port \$port --transport $transport --out /tmp/pair_\$port.bin > /dev/null 2> /tmp/pair_\$port.err ) &
    done
    wait; end=\$(date +%s.%N)
    cat /tmp/pair_pull_*.txt; grep -h quic-ingress /tmp/pair_*.err || true
    echo \"$transport connections=$count wall=\$(echo \"\$end - \$start\" | bc -l | cut -c1-5) total_gbps=\$(echo \"$count * 1073741824 * 8 / (\$end - \$start) / 1000000000\" | bc -l | cut -c1-5) sizes=\$(stat -c %s /tmp/pair_*.bin | tr '\n' ' ')\"
    rm -f /tmp/pair_*.bin" | hide
sleep 1
on send 'cat /tmp/pair_push_*.txt; grep -h quic-egress /tmp/pair_push_*.log || true' | hide
