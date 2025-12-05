# Chamelio

Chamelio is a shared, high-performance, and flexible network stack. Guests
can upload and run custom network protocols on the host with Chamelio.

The usual workflow to implement a new Chamelio protocol is to create
a protocol specific slow-path that uses the Chamelio library in
`cham_lib.h` to register protocols, create queues, and
allocate maps in Chamelio. The protocol implementer will then write
an application library that applications in the guest can use
to send and receive data.

## Building

Use the provided [Dev Container](https://code.visualstudio.com/docs/devcontainers/containers) 
in this repo to setup your environment. Dev Containers set up a namespace and let you use
the provided container with all the dependencies built as a development environment.
To use Dev Containers you need to install [Docker](https://www.docker.com/) 
and then install the Dev Containers VSCode
extension. After installation add your user to the docker group. 

```
sudo groupadd docker
sudo usermod -aG docker $USER
newgrp docker
```

To open the repository folder in a container select "Dev Containers: Open Folder in Container""
from the VSCode quick action status bar item (Ctrl + Shift + P).

After the container finished building compile Chamelio inside the container
```
meson setup build
cd build
ninja
```

## Installing Libraries

To install Chamelio libraries system-wide, use the install target:

```
cd build
sudo ninja install
```

This installs:
- **Shared libraries**: libcham.so libcham_utils.so, libcham_udp.so (in `/usr/local/lib`)
- **Headers**: chamelio/*.h files (in `/usr/local/include/chamelio`)
- **pkg-config files**: For dependency resolution (in `/usr/local/lib/pkgconfig`)

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

## Using Chamelio Libraries in External Projects

### Option 1: Using pkg-config (Recommended)

For projects using pkg-config or Meson, you can discover and link Chamelio libraries:

```bash
# Check available libraries
pkg-config --list-all | grep cham

# Get compiler flags
pkg-config --cflags --libs cham           # For Chamelio library
pkg-config --cflags --libs cham_utils     # For utilities library
pkg-config --cflags --libs cham_udp       # For UDP protocol library
```

#### Using with Meson

In your `meson.build`:

```meson
# Simple library usage
project('my_app', 'c')

cham_dep = dependency('cham')              # Chamelio library
cham_utils_dep = dependency('cham_utils')  # Utilities library
cham_udp_dep = dependency('cham_udp')      # UDP protocol library

executable('my_app',
  'main.c',
  dependencies : [cham_udp_dep]  # Automatically includes cham and cham_utils
)
```

### Option 2: Manual Compilation

If not using pkg-config, manually specify include and library paths:

```bash
gcc -I/usr/local/include/chamelio -L/usr/local/lib \
  my_app.c -o my_app -lcham -lcham_utils -lcham_udp
```

## Library Reference

The Chamelio project exposes three main library groups:

### 1. **Utils Library** (`libcham_utils.so`)
Core utility functions used throughout Chamelio and available for applications:
- Queue management
- Shared memory allocation
- Logging
- Scheduler
- Timeout manager
- Clock utilities
- UX socket support
- Packet headers

**Include:** `#include <chamelio/utils.h>` (though individual headers are available)

**Use case:** Applications needing shared memory queues, scheduling, or logging capabilities

### 2. **Chamelio Library** (`libcham.so`)
Host-side library for protocol registration and management:
- Protocol registration
- Queue creation and management
- Map allocation
- Fast-path management

**Includes:** `#include <chamelio/cham_lib.h>`, `#include <chamelio/cham_fast.h>`

**Dependencies:** cham_utils

**Use case:** Protocol implementations on the guest

### 3. **Protocol Libraries**
Protocol-specific libraries for applications to send/receive data.

#### UDP Protocol Library (`libcham_udp.so`)
Example UDP protocol library for use by guest applications:
- UDP packet sending/receiving
- UDP socker creation and management

**Include:** `#include <chamelio/udp_lib.h>`

**Dependencies:** cham, cham_utils

**Use case:** Guest applications needing UDP protocol support via Chamelio

## Tests

Chamelio tests are located in the `tests/` directory. To run all the tests
execute the command `meson test -C build -v` from the Chamelio directory.
Note that some tests start Chamelio, which needs sudo access.
To run an invididual test suite execute `meson test -C build test-name -v`

Chamelio tests
- `lib-test`: Tests the library functions
- `scheduler-test`: Tests scheduler that implements a priority list
- `queue-test`: Tests shared memory queues
- `arp-test` : Tests arp table inserts and lookups
- `tomgr-test` : Tests the timeout manager
