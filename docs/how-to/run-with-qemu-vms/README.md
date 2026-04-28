# Run Chamelio with QEMU VMs

In a VM deployment, the host runs Chamelio and each guest runs the protocol
slow path plus the application. Chamelio and QEMU communicate over the
`ivshmem-doorbell` socket at `/run/chamelio/ivshmem_socket`.

Start order matters:

1. Prepare host hugepages and NIC.
2. Start host Chamelio.
3. Boot the guest VM with the ivshmem device.
4. Bind the ivshmem device to VFIO in the guest.
5. Start the protocol slow path in the guest with `--virt`.
6. Start the application in the guest.

## Host: Start Chamelio

```bash
sudo ./build/chamelio/chamelio \
  --ip-addr=192.168.10.14/24 \
  --fp-cores-max=1 \
  --shm-len=1073741824 \
  --cham-queue-len=16384 \
  --control-txq-len=1024 \
  --control-txq-pkt-len=1500 \
  --virt-gre \
  --virt-path=chamelio/netvirt/netvirt_conf.csv \
  --dpdk-extra="-a0000:d8:00.0" \
  --dpdk-extra="--lcores=0@1,1@3"
```

Use a network virtualization config that maps each guest ID and inner IP to the
outer Chamelio IP. See [Network virtualization config](../../reference/netvirt-config/README.md).

## Host: Boot the Guest

```bash
qemu-system-x86_64 \
  -snapshot \
  -nographic -monitor none -serial stdio \
  -machine accel=kvm,type=q35 \
  -cpu host \
  -smp 8 \
  -m 8G \
  -device virtio-net-pci,netdev=net0 \
  -netdev user,id=net0,hostfwd=tcp::2220-:22 \
  -chardev socket,path=/run/chamelio/ivshmem_socket,id=cham \
  -device ivshmem-doorbell,vectors=1,chardev=cham \
  -drive if=virtio,format=qcow2,file=/path/to/base-cham.qcow2 \
  -drive if=virtio,format=raw,file=/path/to/seed-cham.img
```

## Guest: Bind ivshmem and Start the Slow Path

Inside the VM:

```bash
sudo modprobe vfio
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
echo "1af4 1110" | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id
```

Start UDP:

```bash
cd /workspaces/chamelio
sudo ./build/protos/udp/slow/udp_slow \
  --virt \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

Start TCP:

```bash
cd /workspaces/chamelio
sudo ./build/protos/tcp/slow/tcp_slow \
  --virt \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

## Guest: Start Applications

Run applications exactly as on bare metal, but bind to the guest inner IP from
the network virtualization config.

Example UDP server inside a server VM:

```bash
cd /workspaces/benchmarks
./build/udp/udp_cham_server 10.0.0.1 1234 --msize 64 --ncores 1 --buf-size 64
```

Example UDP client inside a client VM:

```bash
cd /workspaces/benchmarks
./build/udp/udp_cham_client 10.0.0.60 1234 10.0.0.1 1234 \
  --msize 64 --rate 10000 --ncores 1 --duration 30
```

For TCP, use the same inner IP rule with `tcp_cham_server` and
`tcp_cham_client`.
