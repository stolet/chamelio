# Chamelio

Chamelio is a shared, high-performance, and flexible network stack. Guests
can upload and run custom network protocols on the host with Chamelio.

The usual workflow to implement a new Chamelio protocol is to create
a protocol specific slow-path that uses the Chamelio library in
`cham_lib.h` to register, create queues, and
allocate maps in Chamelio. The protocol implementer will then write
an application library that applications in the guest can use
to send and receive data.

## Building

Requirements
  * [DPDK](https://www.dpdk.org/) v25
  * [Meson](https://mesonbuild.com/) > v1.0.0

To build Chamelio run the following commands
```
meson setup build
cd build
ninja
```

## Running Chamelio

Before running Chamelio the following steps are necessary
  * Make sure `hugetlbfs` is mounted on `/dev/hugepages` and enough huge pages are
    allocated for Chamelio and DPDK.
  * Bind the NIC to the DPDK driver. For Mellanox NICs you can skip the bind step.

```
sudo modprobe vfio-pci
sudo mount -t hugetlbfs nodev /dev/hugepages
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
sudo python3 ~/dpdk-inst/usertools/dpdk-devbind.py  -b vfio-pci 0000:08:00.0
```

You can start Chamelio on the host with the following command
```
sudo code/build/chamelio/chamelio --ip-addr=192.168.10.1/24 --fp-cores-max=1 --dpdk-extra="-ad8:00.0"
```
The `-a` dpdk-extra arg is the PCI ID of the NIC port to be used by DPDK.

## Running Slow-path

This repo has some examples of different slow-path protocol implementations in the
`protos/` directory. For example, you can run the UDP slow-path after starting Chamelio.

```
sudo code/build/protos/udp/slow/udp_slow
```

This registers a slow-path and creates a shared memory region to be
used by the protocol, application and Chamelio.

## Running Applications

Applications can use custom protocols registed with Chamelio by using the
libraries exposed by the custom protocol. For example, `code/protos/udp/lib`
exposes a library that can be used by applications to send and receive
messages using an example Chamelio UDP protocol.

Example applications that use these libraries can be found in `code/examples`.

## Tests

Chamelio tests are located in the `tests/` directory. To run all the tests
execute the command `meson test -C build -v` from the Chamelio directory.
Note that some tests need to start Chamelio, which needs sudo access.
To run an invididual test suite execute `meson test -C build test-name -v`

Chamelio tests
- `test_cham_lib`: Tests the chamelio library function
- `test_cham_scheduler`: Tests Chamelio scheduler that implements a priority list
