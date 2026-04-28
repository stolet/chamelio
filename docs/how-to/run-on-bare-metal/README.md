# Run Chamelio on Bare Metal

This guide shows the minimum runtime setup for a physical host. Repeat it on
each machine that participates in a Chamelio run.

## Prepare Hugepages

```bash
sudo modprobe vfio-pci
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs nodev /dev/hugepages
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
```

If `/dev/hugepages` is already mounted, the mount command may fail with
`already mounted`; that is not a problem.

## Bind a NIC for DPDK

Find the PCI address:

```bash
sudo dpdk-devbind.py --status
```

Bind the NIC:

```bash
sudo ip link set <kernel_ifname> down
sudo dpdk-devbind.py -b vfio-pci <pci_bdf>
```

Example:

```bash
sudo ip link set ens6f0 down
sudo dpdk-devbind.py -b vfio-pci 0000:d8:00.0
```

Some NICs, including common Mellanox setups, can be used by DPDK without
binding away from the kernel driver. In that case, keep the interface usable by
the PMD and still pass its PCI address to Chamelio.

## Start Chamelio

```bash
sudo ./build/chamelio/chamelio \
  --ip-addr=<chamelio_ip>/<prefix> \
  --fp-cores-max=1 \
  --shm-len=1073741824 \
  --cham-queue-len=16384 \
  --control-txq-len=1024 \
  --control-txq-pkt-len=1500 \
  --dpdk-extra="-a<pci_bdf>" \
  --dpdk-extra="--lcores=0@1,1@3"
```

Example:

```bash
sudo ./build/chamelio/chamelio \
  --ip-addr=192.168.10.14/24 \
  --fp-cores-max=1 \
  --dpdk-extra="-a0000:d8:00.0" \
  --dpdk-extra="--lcores=0@1,1@3"
```

`--lcores=0@1,1@3` assigns DPDK lcore `0` to CPU `1` and Chamelio's first
fast-path lcore to CPU `3`. Keep those CPUs away from application threads.

## Start a Protocol Slow Path

UDP:

```bash
sudo ./build/protos/udp/slow/udp_slow \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

TCP:

```bash
sudo ./build/protos/tcp/slow/tcp_slow \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

Start Chamelio before the slow path. Start the application after the slow path.

## Run an Application

Applications must be linked with the protocol library and must connect to the
slow path before creating sockets:

- UDP: `udp_connect_slow()`, then `udp_ctx_new()`.
- TCP: `tcp_connect_slow()`, then `tcp_ctx_new()`.

See [Use UDP in an application](../use-udp-in-an-application/README.md) and
[Use TCP in an application](../use-tcp-in-an-application/README.md).
