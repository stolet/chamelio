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

## 1. Start the Container

On both machines, build the Chamelio container image from the repository root:

```bash
cd /local/mstolet/projects/chamelio
docker image build -t chamelio-dev:latest -f .devcontainer/Dockerfile .
```

Start a long-running container. Mount the benchmark repository if you want to
run the echo benchmark from this tutorial:

```bash
docker container rm -f chamelio-dev >/dev/null 2>&1 || true
docker container run -d \
  --name chamelio-dev \
  --privileged \
  --network=host \
  --ulimit memlock=-1:-1 \
  --workdir /workspaces/chamelio \
  --mount type=bind,source=/local/mstolet/projects/chamelio,target=/workspaces/chamelio \
  --mount type=bind,source=/local/mstolet/projects/chamelio-benchmarks/benchmarks,target=/workspaces/benchmarks \
  --mount type=bind,source=/dev/hugepages,target=/dev/hugepages \
  chamelio-dev:latest sleep infinity
```

Enter the container:

```bash
docker container exec -it chamelio-dev bash
```

Use additional `docker container exec -it chamelio-dev bash` shells for the
Chamelio, TCP slow-path, and application processes.

## 2. Build and Install

Inside the container on both machines:

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

## 3. Prepare Hugepages and the NIC

On the host or inside the privileged container on both machines:

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

## 4. Start Chamelio

Inside the container on the server:

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

Inside the container on the client:

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

## 5. Start the TCP Slow Path

Inside the container on both machines:

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

## 6. Start the Echo Server

Inside the container on the server:

```bash
cd /workspaces/benchmarks
./build/tcp/tcp_cham_server \
  192.168.10.14 1234 \
  --msize 64 \
  --ncores 1 \
  --buf-size 64 \
  --mconns 1
```

## 7. Start the Echo Client

Inside the container on the client:

```bash
cd /workspaces/benchmarks
./build/tcp/tcp_cham_client \
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

## 8. Stop the Run

Stop processes in this order:

1. client application
2. server application
3. TCP slow paths
4. Chamelio processes

Then remove the container:

```bash
docker container rm -f chamelio-dev
```

If Chamelio fails during startup, use
[Troubleshoot runtime setup](../../how-to/troubleshoot-runtime-setup/README.md).
