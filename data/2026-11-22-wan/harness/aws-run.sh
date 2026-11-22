#!/bin/bash
# Two-host AWS run of embr's WAN benchmark: launch, install, run bench/wan.sh, fetch, tear down.
# Everything it creates is recorded in ./state and removed by "down"; nothing else in the account is touched.
#   run.sh check | up | install | tests | key | smoke | full | status | fetch <data dir> | extend <minutes> | down
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
STATE=$here/state
KEYFILE=$here/desktop-key
PROFILE=${AWS_PROFILE:-default}
SEND_REGION=${SEND_REGION:-us-east-1}
RECV_REGION=${RECV_REGION:-us-east-2}
TYPE=${TYPE:-c5n.large}
LIFETIME_MIN=${LIFETIME_MIN:-180}
REPO_URL=${REPO_URL:-https://github.com/chenbenny31/embr.git}

a()      { aws --profile "$PROFILE" --output text "$@"; }
put()    { echo "$1=$2" >> "$STATE"; }
get()    { sed -n "s/^$1=//p" "$STATE" 2>/dev/null | tail -1; }
need()   { local v; v=$(get "$1"); [[ -n "$v" ]] || { echo "no $1 in the state file" >&2; exit 1; }; echo "$v"; }
region() { [[ "$1" == send ]] && echo "$SEND_REGION" || echo "$RECV_REGION"; }
SSH_OPTS=(-i "$KEYFILE" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile="$here/known_hosts"
          -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes -o ServerAliveInterval=15)
on()     { local role=$1; shift; ssh "${SSH_OPTS[@]}" "ec2-user@$(need "${role}_ip")" "$@"; }

# <region> -> "kernel name image-id root-device" of the Amazon Linux 2023 image with the newest kernel
ami() {
    a ec2 describe-images --region "$1" --owners amazon \
        --filters 'Name=name,Values=al2023-ami-2023.*-kernel-*-x86_64' 'Name=state,Values=available' \
        --query 'Images[].[Name,ImageId,RootDeviceName]' |
        awk '{ k = $1; sub(/.*-kernel-/, "", k); sub(/-x86_64$/, "", k); print k, $1, $2, $3 }' |
        sort -k1,1V -k2,2V | tail -1
}

# <region> -> first availability zone that offers the instance type
zone() {
    a ec2 describe-instance-type-offerings --region "$1" --location-type availability-zone \
        --filters "Name=instance-type,Values=$TYPE" --query 'InstanceTypeOfferings[].Location' |
        tr '\t' '\n' | sort | head -1
}

cmd_check() {
    a sts get-caller-identity --query Account > /dev/null && echo "credentials: accepted"
    local role r image kernel image_name image_id root_dev out
    for role in send recv; do
        r=$(region "$role"); image=$(ami "$r" || true)
        read -r kernel image_name image_id root_dev <<< "$image" || true
        echo "$role $r: image ${image_name:-NONE} (kernel ${kernel:-?}), zone $(zone "$r" || echo NONE)," \
             "default network $(a ec2 describe-vpcs --region "$r" --filters Name=is-default,Values=true --query 'length(Vpcs)' 2>&1 | tail -1)"
        [[ -n "${image_id:-}" ]] || continue
        out=$(a ec2 run-instances --region "$r" --dry-run --image-id "$image_id" --instance-type "$TYPE" 2>&1 || true)
        echo "  launch check: $(sed -n 's/.*(\([A-Za-z.]*\)).*/\1/p' <<< "$out" | head -1) (DryRunOperation means allowed)"
    done
}

cmd_up() {
    [[ -e "$STATE" ]] && { echo "a state file exists: run down first" >&2; exit 1; }
    local name my_ip role r kernel image_name image_id root_dev group id port
    name="embr-wan-$(date -u +%Y%m%d-%H%M)"
    my_ip=$(curl -s --max-time 10 https://checkip.amazonaws.com)
    [[ "$my_ip" =~ ^[0-9.]+$ ]] || { echo "cannot find this machine's public address" >&2; exit 1; }
    put name "$name"
    ssh-keygen -q -t ed25519 -N '' -C "$name" -f "$KEYFILE"
    # safety net: the instance ends itself (shutdown behaviour is terminate) even if nobody comes back
    printf '#!/bin/bash\nshutdown -h +%s --no-wall\n' "$LIFETIME_MIN" > "$here/userdata.sh"
    for role in send recv; do
        r=$(region "$role"); read -r kernel image_name image_id root_dev <<< "$(ami "$r")"
        a ec2 import-key-pair --region "$r" --key-name "$name" --public-key-material "fileb://$KEYFILE.pub" --query KeyName > /dev/null
        put "${role}_key" "$name"
        group=$(a ec2 create-security-group --region "$r" --group-name "$name" --description "embr WAN benchmark, temporary" --query GroupId)
        put "${role}_group" "$group"
        a ec2 authorize-security-group-ingress --region "$r" --group-id "$group" --protocol tcp --port 22 --cidr "$my_ip/32" > /dev/null
        id=$(a ec2 run-instances --region "$r" --image-id "$image_id" --instance-type "$TYPE" --key-name "$name" \
            --security-group-ids "$group" --placement "AvailabilityZone=$(zone "$r")" \
            --instance-initiated-shutdown-behavior terminate --user-data "file://$here/userdata.sh" \
            --block-device-mappings "DeviceName=$root_dev,Ebs={VolumeSize=20,VolumeType=gp3,DeleteOnTermination=true}" \
            --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$name-$role}]" \
            --query 'Instances[0].InstanceId')
        put "${role}_id" "$id"; put "${role}_image" "$image_name"
        echo "$role: launched in $r from $image_name"
    done
    for role in send recv; do
        r=$(region "$role")
        a ec2 wait instance-running --region "$r" --instance-ids "$(need "${role}_id")"
        put "${role}_ip" "$(a ec2 describe-instances --region "$r" --instance-ids "$(need "${role}_id")" \
            --query 'Reservations[0].Instances[0].PublicIpAddress')"
    done
    # what the receiver needs on the sender: ssh and the scp row (22), embr TCP (10007), nc (9999),
    # one UDP port per QUIC row (10008-10011), and echo for the round-trip record
    group=$(need send_group); id=$(need recv_ip)
    for port in 22 10007 9999; do
        a ec2 authorize-security-group-ingress --region "$SEND_REGION" --group-id "$group" --protocol tcp --port "$port" --cidr "$id/32" > /dev/null
    done
    a ec2 authorize-security-group-ingress --region "$SEND_REGION" --group-id "$group" --protocol udp --port 10008-10011 --cidr "$id/32" > /dev/null
    a ec2 authorize-security-group-ingress --region "$SEND_REGION" --group-id "$group" --protocol icmp --port -1 --cidr "$id/32" > /dev/null
    for role in send recv; do
        for _ in $(seq 60); do on "$role" true 2>/dev/null && break; sleep 5; done
        echo "$role: up, kernel $(on "$role" uname -r), $(on "$role" nproc) CPUs"
    done
}

cmd_install() {
    local role send_rc recv_rc
    for role in send recv; do
        on "$role" "sudo dnf install -y -q git > /dev/null 2>&1; rm -f install.rc; git clone -q $REPO_URL embr && cd embr &&
            ( nohup setsid bash -c 'BUILD_EMBR=1 bash install.sh > ~/install.log 2>&1; echo \$? > ~/install.rc' > /dev/null 2>&1 < /dev/null & )"
    done
    for _ in $(seq 300); do
        send_rc=$(on send 'cat install.rc 2>/dev/null' || true); recv_rc=$(on recv 'cat install.rc 2>/dev/null' || true)
        [[ -n "$send_rc" && -n "$recv_rc" ]] && break
        sleep 10
    done
    for role in send recv; do
        echo "$role: install exit status $(on "$role" 'cat install.rc 2>/dev/null || echo none'), commit $(on "$role" 'git -C embr rev-parse --short HEAD'), $(on "$role" 'ls embr/build/embr 2>/dev/null || echo "no embr binary"')"
    done
    [[ "$send_rc" == 0 && "$recv_rc" == 0 ]] || { on send 'tail -25 install.log'; on recv 'tail -25 install.log'; exit 1; }
}

cmd_tests() {
    local role
    for role in send recv; do echo "$role: $(on "$role" 'cd embr && ./build/embr_test 2>&1 | tail -3' | tr '\n' ' ')"; done
}

cmd_key() { # the receiver gets its own key for the sender
    on recv 'test -f ~/.ssh/bench || ssh-keygen -q -t ed25519 -N "" -f ~/.ssh/bench; cat ~/.ssh/bench.pub' | on send 'cat >> ~/.ssh/authorized_keys'
    on recv "ssh -i \$HOME/.ssh/bench -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=10 ec2-user@$(need send_ip) true" &&
        echo "the receiver reaches the sender over ssh"
}

cmd_smoke() { on recv "cd embr && SENDER_IP=$(need send_ip) SSH_KEY=\$HOME/.ssh/bench RUNS=1 WARMUP=1 RETRY_MAX=2 PASSES=default bash bench/wan.sh"; }
cmd_full()  { on recv "cd embr && SENDER_IP=$(need send_ip) SSH_KEY=\$HOME/.ssh/bench ${WAN_ENV:-} bash bench/wan.sh"; }

cmd_status() {
    on recv 'log=$(ls -t ~/embr-wan/*.log 2>/dev/null | head -1); echo "log: ${log##*/}"; grep -E "^\[wan\]|FAILED|FATAL" "$log" | tail -8; tail -2 "$log"; ls ~/embr-wan/*.tar.gz 2>/dev/null | sed "s|.*/||"'
    for role in send recv; do echo "$role: $(on "$role" 'uptime -p; df -h --output=avail / | tail -1' | tr '\n' ' ')"; done
}

cmd_fetch() { # newest tarball on the receiver -> <data dir>/<date>-wan/
    local data=$1 tarball stamp dest
    tarball=$(on recv 'ls -t ~/embr-wan/embr-wan-*.tar.gz | head -1')
    stamp=${tarball##*/embr-wan-}; stamp=${stamp%.tar.gz}
    dest=$data/${stamp%-*}-wan${SUFFIX:-}
    [[ -e "$dest" ]] && { echo "$dest exists" >&2; exit 1; }
    mkdir -p "$here/fetched" "$dest"
    scp -q "${SSH_OPTS[@]}" "ec2-user@$(need recv_ip):$tarball" "$here/fetched/"
    tar -xzf "$here/fetched/${tarball##*/}" -C "$dest" --strip-components=1
    echo "$dest: $(find "$dest" -type f | wc -l) files, $(du -sh "$dest" | cut -f1)"
}

cmd_extend() { # push the self-shutdown back: <minutes from now>
    local role
    for role in send recv; do on "$role" "sudo shutdown -c --no-wall; sudo shutdown -h +$1 --no-wall" 2>&1 | tail -1; done
}

cmd_down() {
    local role r id group key left
    for role in send recv; do
        r=$(region "$role"); id=$(get "${role}_id")
        [[ -n "$id" ]] && { a ec2 terminate-instances --region "$r" --instance-ids "$id" --query 'TerminatingInstances[0].CurrentState.Name' || echo "FAILED: terminate $role"; }
    done
    for role in send recv; do
        r=$(region "$role"); id=$(get "${role}_id"); group=$(get "${role}_group"); key=$(get "${role}_key")
        [[ -n "$id" ]] && { a ec2 wait instance-terminated --region "$r" --instance-ids "$id" || echo "FAILED: wait for $role"; }
        [[ -n "$group" ]] && { a ec2 delete-security-group --region "$r" --group-id "$group" > /dev/null || echo "FAILED: security group in $r"; }
        [[ -n "$key" ]] && { a ec2 delete-key-pair --region "$r" --key-name "$key" > /dev/null || echo "FAILED: key pair in $r"; }
        left=$(a ec2 describe-instances --region "$r" --filters "Name=tag:Name,Values=$(get name)-*" \
            "Name=instance-state-name,Values=pending,running,stopping,stopped" --query 'length(Reservations[].Instances[])' || echo unknown)
        echo "$role $r: instances of this run still alive: $left"
    done
    rm -f "$KEYFILE" "$KEYFILE.pub" "$here/known_hosts" "$here/userdata.sh"
    mv "$STATE" "$STATE.done-$(date -u +%H%M%S)"
}

cmd=${1:-}; shift || true
case "$cmd" in
    check|up|install|tests|key|smoke|full|status|fetch|extend|down) "cmd_$cmd" "$@" ;;
    *) sed -n '2,4p' "$0"; exit 2 ;;
esac
