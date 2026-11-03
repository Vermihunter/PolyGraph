#!/usr/bin/env bash
set -u

echo '=== GPU topology / peer access ==='
nvidia-smi topo -m || true
echo
nvidia-smi topo -p2p r || true

echo '=== InfiniBand / RoCE devices ==='
if command -v ibv_devinfo >/dev/null 2>&1; then
  ibv_devinfo || true
else
  echo 'ibv_devinfo not installed'
fi

echo '=== UCX transports ==='
if command -v ucx_info >/dev/null 2>&1; then
  ucx_info -d || true
else
  echo 'ucx_info not installed'
fi

echo '=== NVIDIA peer-memory / DMA-BUF clues ==='
lsmod | grep -E 'nvidia_peermem|ib_core|mlx5' || true
for f in /sys/kernel/mm/memory_peers/nv_mem/version /proc/driver/nvidia/gpus/*/information; do
  [ -r "$f" ] && { echo "--- $f"; cat "$f"; }
done

echo '=== GPU / NIC PCI devices ==='
lspci -nn | grep -Ei 'NVIDIA|Mellanox|Ethernet|InfiniBand' || true
