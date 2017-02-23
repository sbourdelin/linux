#!/bin/bash
local_dir="$(pwd)"
root_dir=$local_dir/../..
mnt_dir=/sys/fs/bpf

on_exit() {
  iptables -D INPUT -m bpf --object-pinned ${mnt_dir}/bpf_prog -j ACCEPT
  umount ${mnt_dir}
}

trap on_exit EXIT
mkdir -p ${mnt_dir}
mount -t bpf bpf ${mnt_dir}
./perSocketStats_example ${mnt_dir}/bpf_prog
