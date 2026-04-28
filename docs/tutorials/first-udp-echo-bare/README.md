# First UDP Echo Run on Bare Metal

This tutorial starts Chamelio UDP on two physical machines and runs the UDP
echo benchmark. Use it after the build succeeds and before moving to VM or
custom applications.

The example uses:

- server Chamelio IP: `192.168.10.14/24`
- client Chamelio IP: `192.168.10.13/24`
- UDP server port: `1234`
- one fast-path core per host
- one application core per host

Replace PCI addresses and CPU cores with values from your machines.

## 1. Build and Install

On both machines:

```bash
cd /workspaces/chamelio
meson setup build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

Build the benchmarks:

```bash
cd /workspaces/benchmarks
meson setup build
ninja -C build udp_cham
```

## 2. Prepare Hugepages and the NIC

On both machines:

```bash
sudo modprobe vfio-pci
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs nodev /dev/hugepages
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
sudo dpdk-devbind.py -b vfio-pci 0000:d8:00.0
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

## 4. Start the UDP Slow Path

On both machines:

```bash
cd /workspaces/chamelio
sudo ./build/protos/udp/slow/udp_slow \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

## 5. Start the Echo Server

On the server:

```bash
cd /workspaces/benchmarks
taskset -c 7,9 ./build/udp/udp_cham_server \
  192.168.10.14 1234 \
  --msize 64 \
  --ncores 1 \
  --buf-size 64
```

## 6. Start the Echo Client

On the client:

```bash
cd /workspaces/benchmarks
taskset -c 7,9 ./build/udp/udp_cham_client \
  192.168.10.13 1234 192.168.10.14 1234 \
  --msize 64 \
  --rate 10000 \
  --ncores 1 \
  --duration 30
```

The client prints throughput and latency percentiles once per second. The UDP
benchmark sends a small amount of initial traffic to resolve ARP before the
main run begins.

## 7. Stop the Run

Stop the client, server, UDP slow paths, and Chamelio processes.

If packets do not flow after the processes start, check ARP and NIC binding in
[Troubleshoot runtime setup](../../how-to/troubleshoot-runtime-setup/README.md).
