# First TCP Echo Run on Bare Metal

This tutorial starts Chamelio TCP on two physical machines and runs the TCP
echo benchmark. Use it to validate a fresh setup before changing core counts,
queues, or applications.

The example uses:

- server Chamelio IP: `192.168.10.14/24`
- client Chamelio IP: `192.168.10.13/24`
- TCP server port: `1234`
- one fast-path core per host
- one application core per host

Replace PCI addresses and CPU cores with values from your machines.

## 1. Build and Install

On both machines, inside the Chamelio development environment:

```bash
cd /workspaces/chamelio
meson setup build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

Build the benchmarks after Chamelio is installed:

```bash
cd /workspaces/benchmarks
meson setup build
ninja -C build tcp_cham
```

## 2. Prepare Hugepages and the NIC

On both machines:

```bash
sudo modprobe vfio-pci
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs nodev /dev/hugepages
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
```

Bind the NIC used by Chamelio to a DPDK-compatible driver. Skip this for NICs
whose DPDK PMD uses the kernel driver directly (e.g. Mellanox).

```bash
sudo dpdk-devbind.py -b vfio-pci 0000:d8:00.0
sudo dpdk-devbind.py --status
```

## 3. Start Chamelio

On the server:

```bash
cd /workspaces/chamelio
sudo ./build/chamelio/chamelio \
  --ip-addr=192.168.10.14/24 \
  --fp-cores-max=1 \
  --shm-len=1073741824 \
  --cham-queue-len=16384 \
  --control-txq-len=1024 \
  --control-txq-pkt-len=1500 \
  --dpdk-extra="-a0000:d8:00.0" \
  --dpdk-extra="--lcores=0@1,1@3"
```

On the client:

```bash
cd /workspaces/chamelio
sudo ./build/chamelio/chamelio \
  --ip-addr=192.168.10.13/24 \
  --fp-cores-max=1 \
  --shm-len=1073741824 \
  --cham-queue-len=16384 \
  --control-txq-len=1024 \
  --control-txq-pkt-len=1500 \
  --dpdk-extra="-a0000:d8:00.0" \
  --dpdk-extra="--lcores=0@1,1@3"
```

Leave both Chamelio processes running.

## 4. Start the TCP Slow Path

On both machines:

```bash
cd /workspaces/chamelio
sudo ./build/protos/tcp/slow/tcp_slow \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

The slow path registers TCP with the local Chamelio instance and waits for
applications.

## 5. Start the Echo Server

On the server:

```bash
cd /workspaces/benchmarks
taskset -c 7,9 ./build/tcp/tcp_cham_server \
  192.168.10.14 1234 \
  --msize 64 \
  --ncores 1 \
  --buf-size 64 \
  --mconns 1
```

## 6. Start the Echo Client

On the client:

```bash
cd /workspaces/benchmarks
taskset -c 7,9 ./build/tcp/tcp_cham_client \
  -1 -1 192.168.10.14 1234 \
  --msize 64 \
  --rate 999999999 \
  --ncores 1 \
  --duration 30 \
  --nconns 1 \
  --mpending 1 \
  --cwait 2
```

The client should print per-second throughput and latency statistics. The
server should show receive/transmit progress.

## 7. Stop the Run

Stop processes in this order:

1. client application
2. server application
3. TCP slow paths
4. Chamelio processes

If Chamelio fails during startup, use
[Troubleshoot runtime setup](../../how-to/troubleshoot-runtime-setup/README.md).
