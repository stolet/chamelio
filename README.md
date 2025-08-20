# Chamelio

Chamelio is a shared, high-performance, and flexible network stack. Guests
can run and upload custom protocols to run on Cbamelio in the host.

## Building

Requirements
  * [DPDK](https://www.dpdk.org/) v25
  * [Meson](https://mesonbuild.com/) v1.0.0 <

To build Chamelio run the following commands
```
meson setup build
ninja
```

## Running

Before running Chamelio the following steps are necessary
  * Make sure `hugetlbfs` is mounted on `/dev/hugepages` and enough huge pages are
    allocated for Chamelio and DPDK.
  * Bind the NIC to the DPDK driver. For Mellanox NICs you can skip the bind step.

```
sudo modprobe vfio-pci
sudo mount -t hugetlbfs nodev /dev/hugepages
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
sudo ~/dpdk-inst/usertools/dpdk-devbind  -b vfio-pci 0000:08:00.0
```

You can start Chamelio on the host with the following command
```
sudo code/build/chamelio/chamelio --ip-addr=192.168.10.1/24 --fp-cores-max=1 --dpdk-extra="-ad8:00.0"
```

The `-a` dpdk-extra arg is the PCI ID of the NIC port that you want DPDK to use.
