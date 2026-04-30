# <img src="docs/assets/chamelio.svg" alt="Chamelio logo" height="60" style="vertical-align: text-bottom;"> Chamelio

Chamelio is a shared, high-performance network stack that lets guests and
applications use protocol implementations registered with a host fast path.
A protocol in Chamelio is split into a slow path, an application library, and
fast-path code that Chamelio can run while processing packets.

This repository contains:

- `chamelio/`: the Chamelio executable, control plane, NIC integration, fast
  path, ARP handling, and network virtualization support.
- `lib/`: the Chamelio protocol-development library and shared utilities.
- `protos/`: protocol implementations. TCP and UDP are included.
- `tests/`: unit tests for shared utilities and core tables.

## Documentation

The documentation is organized by the following categories:

- [Tutorials](docs/tutorials/README.md): guided walkthroughs.
- [How-to guides](docs/how-to/README.md): focused recipes for setup, running,
  application integration, and protocol development.
- [Reference](docs/reference/README.md): command options, libraries, public
  APIs, and configuration formats.
- [Design](docs/design/README.md): architecture, runtime model, and protocol internals.

Common entry points:

- [Build and install Chamelio](docs/how-to/build-and-install/README.md)
- [Run Chamelio on bare metal](docs/how-to/run-on-bare-metal/README.md)
- [Set up a QEMU VM for Chamelio](docs/how-to/set-up-a-qemu-vm/README.md)
- [Run Chamelio with QEMU VMs](docs/how-to/run-with-qemu-vms/README.md)
- [Use UDP in an application](docs/how-to/use-udp-in-an-application/README.md)
- [Use TCP in an application](docs/how-to/use-tcp-in-an-application/README.md)
- [Add a new protocol](docs/how-to/add-a-new-protocol/README.md)
- [Debug TCP and UDP state](docs/how-to/debug-with-statetool/README.md)
- [API reference](docs/reference/libraries/README.md)

## Quick Build

The supported development path is the repository container. It provides DPDK,
libbpf, Meson, Ninja, the eBPF compiler, and the compiler toolchain used by
this project.

```bash
docker image build -t chamelio-dev:latest -f .devcontainer/Dockerfile .
docker container run --rm -it \
  --workdir /workspaces/chamelio \
  --mount type=bind,source="$PWD",target=/workspaces/chamelio \
  chamelio-dev:latest bash

meson setup build
ninja -C build
```

Install the libraries and headers when you want external applications or
benchmarks to link against Chamelio:

```bash
sudo ninja -C build install
sudo ldconfig
```

Installed applications can discover libraries with pkg-config:

```bash
pkg-config --cflags --libs cham
pkg-config --cflags --libs cham_udp
pkg-config --cflags --libs cham_tcp
```

## Minimal Runtime Shape

A bare-metal TCP or UDP run has three processes per host:

1. Chamelio, bound to a DPDK NIC.
2. The protocol slow path, such as `protos/tcp/slow/tcp_slow`.
3. An application linked with the protocol library.

A VM run starts Chamelio on the host first, then boots the guest with the
QEMU `ivshmem-doorbell` device connected to Chamelio's ivshmem socket. The
protocol slow path and application then run inside the guest with protocol
virtualization enabled.

Start with [the first TCP echo tutorial](docs/tutorials/first-tcp-echo-bare/README.md)
if you are validating a two-machine bare-metal setup.
