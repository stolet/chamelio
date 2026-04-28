# Troubleshoot Runtime Setup

Use this page when Chamelio or a protocol starts but packets do not flow.

## DPDK Cannot Find the NIC

Check the BDF:

```bash
sudo dpdk-devbind.py --status
lspci -D
```

Make sure the value passed to Chamelio matches the DPDK device:

```bash
--dpdk-extra="-a0000:d8:00.0"
```

If the NIC needs VFIO, bring the kernel interface down and bind it:

```bash
sudo ip link set <ifname> down
sudo modprobe vfio-pci
sudo dpdk-devbind.py -b vfio-pci <pci_bdf>
```

## Hugepage Allocation Fails

Check the mount and free pages:

```bash
mount | grep hugetlbfs
grep -i huge /proc/meminfo
```

Allocate pages on every NUMA node used by Chamelio:

```bash
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
```

Reduce `--shm-len` or `--shm-internal-len` if the machine cannot reserve
enough hugepage memory.

## The Slow Path Cannot Connect

Start order should be:

1. Chamelio.
2. Protocol slow path.
3. Application.

On bare metal, the slow path connects through `/run/chamelio/guest_socket`.
On VM deployments, the slow path uses the ivshmem device and must be started
with `--virt`.

## Applications Cannot Connect to the Slow Path

Make sure the matching slow path is running:

- UDP applications require `udp_slow`.
- TCP applications require `tcp_slow`.

The application process must call:

- UDP: `udp_connect_slow()`
- TCP: `tcp_connect_slow()`

before creating contexts or sockets.

## VM ivshmem Is Missing

Start Chamelio before QEMU. QEMU connects to:

```text
/run/chamelio/ivshmem_socket
```

The QEMU command must include:

```bash
-chardev socket,path=/run/chamelio/ivshmem_socket,id=cham
-device ivshmem-doorbell,vectors=1,chardev=cham
```

Inside the guest, bind the ivshmem device to VFIO:

```bash
sudo modprobe vfio
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
echo "1af4 1110" | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id
```

## Packets Do Not Flow

Check these in order:

1. Chamelio IP addresses match the physical link or network virtualization
   setup.
2. Server application is bound to the address clients are using.
3. UDP/TCP slow path is the same protocol as the application library.
4. ARP has resolved. The benchmark clients and servers send initial traffic to
   trigger ARP.
5. Application cores are not pinned to the same CPUs as DPDK lcores.
6. VM applications use inner IPs from the netvirt config, not outer host IPs.

## `pkg-config` Cannot Find Chamelio

Install and refresh the linker cache:

```bash
sudo ninja -C build install
sudo ldconfig
```

Then, if needed:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```
