# Set Up a QEMU VM for Chamelio

Chamelio uses QEMU's `ivshmem-doorbell` device to connect a guest to the
host-side Chamelio process. The host must start Chamelio first so QEMU can
connect to Chamelio's ivshmem socket.

This guide describes the minimum VM shape. It does not require the experiment
automation from `sigcomm24-artifact`.

## Host Requirements

Install QEMU/KVM and make sure the current user can run KVM:

```bash
sudo apt-get install qemu-system-x86 qemu-utils cloud-image-utils
sudo usermod -aG kvm $USER
```

Create a VM image with:

- a Linux distribution supported by the Chamelio dev environment;
- a user that can run `sudo`;
- Docker or the dependencies needed to build and run Chamelio applications;
- this repository and any application repository you plan to run.

## Start Chamelio on the Host

Start Chamelio on the host before QEMU:

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

Use `--virt-gre` when packets need GRE-based network virtualization between
host and guest addresses. Omit it only when your experiment does not use the
network virtualization path.

## Boot the VM

This is the minimal QEMU shape for a Chamelio guest using ivshmem:

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

SSH into the VM through the forwarded port:

```bash
ssh -p 2220 tas@localhost
```

## Enable VFIO Access to ivshmem in the Guest

Inside the VM:

```bash
sudo modprobe vfio
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
echo "1af4 1110" | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id
```

If the ivshmem device is already visible as a PCI function, bind that BDF:

```bash
lspci -D
echo vfio-pci | sudo tee /sys/bus/pci/devices/<ivshmem_bdf>/driver_override
echo <ivshmem_bdf> | sudo tee /sys/bus/pci/drivers/vfio-pci/bind
```

The Chamelio VFIO helper defaults to `0000:00:03.0`, which matches the QEMU
command above when the ivshmem device is placed at address `0x3`.

## Prepare Hugepages in the Guest

If the protocol slow path or application uses DPDK-linked libraries in the VM,
prepare hugepages there too:

```bash
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs nodev /dev/hugepages
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
```

Continue with [Run Chamelio with QEMU VMs](../run-with-qemu-vms/README.md).
