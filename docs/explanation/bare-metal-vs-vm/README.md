# Bare Metal vs VM

Chamelio supports two deployment shapes. The same protocol implementation can
run in both, but the connection path and addresses are different.

## Bare Metal

In a bare-metal run, Chamelio, the protocol slow path, and the application run
as host processes on the same machine.

The protocol slow path connects to:

```text
/run/chamelio/guest_socket
```

The slow path calls `cham_connect_guest()` and then
`cham_new_proto_bare()`. Applications use the protocol library and connect to
the protocol slow path.

Use this mode for the simplest validation and for direct host applications.

## VM

In a VM run, Chamelio runs on the host and the protocol slow path plus
application run in a guest VM.

QEMU connects to:

```text
/run/chamelio/ivshmem_socket
```

The guest sees an ivshmem PCI device. The protocol slow path calls
`cham_new_proto_virt()` and must be started with `--virt`.

Use this mode when the application should run inside a guest while sharing the
host Chamelio fast path.

## Addressing Difference

Bare-metal applications normally bind to the Chamelio host IP, such as
`192.168.10.14`.

VM applications normally bind to a guest inner IP, such as `10.0.0.1`. With
`--virt-gre`, Chamelio maps the inner IP to the host outer IP using the
network virtualization CSV.

## Operational Difference

Bare metal needs:

- host hugepages;
- DPDK NIC binding or a DPDK-compatible NIC driver;
- Chamelio, slow path, and app on the same host.

VM mode needs those host requirements plus:

- QEMU/KVM;
- the ivshmem device in the QEMU command line;
- VFIO binding for the ivshmem device inside the guest;
- protocol slow paths started with `--virt`;
- netvirt configuration when inner and outer IPs differ.
