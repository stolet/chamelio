# Build and Install Chamelio

This guide builds the repository and installs Chamelio libraries so external
applications can use them.

## Start the Container

The easiest supported environment is the repository container. It includes the
DPDK, libbpf, Meson, Ninja, Prevail, llvmbpf, and `ecc` dependencies expected
by the build.

If you use VS Code, install the Dev Containers extension, open this repository,
and run `Dev Containers: Open Folder in Container`.

You can also start the same environment directly with Docker.

Build the image from the repository root:

```bash
docker build -t chamelio-dev -f .devcontainer/Dockerfile .
```

Prepare hugepages on the host before starting runtime processes:

```bash
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs nodev /dev/hugepages
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
```

Start a long-running container:

```bash
docker run -d --name chamelio-dev \
  --privileged \
  --network=host \
  -v "$PWD":/workspaces/chamelio \
  -v /dev/hugepages:/dev/hugepages \
  chamelio-dev sleep infinity
```

Enter it:

```bash
docker exec -it chamelio-dev bash
```

Open additional shells with the same `docker exec` command when you need
separate terminals for Chamelio, a slow path, and an application.

## Build

```bash
cd /workspaces/chamelio
meson setup build
ninja -C build
```

If `build/` already exists:

```bash
ninja -C build
```

Clean the build tree:

```bash
ninja -C build clean
```

## Install Libraries and Headers

Install when you need to build applications outside this repository:

```bash
sudo ninja -C build install
sudo ldconfig
```

Installed libraries include:

- `libcham.so`
- `libcham_utils.so`
- `libcham_udp.so`
- `libcham_tcp.so`

Installed headers live under the `chamelio` include subdirectory, for example:

- `chamelio/cham_lib.h`
- `chamelio/cham_fast.h`
- `chamelio/udp_lib.h`
- `chamelio/tcp_lib.h`
- `chamelio/queue.h`
- `chamelio/queue_fns.h`
- `chamelio/queue_types.h`
- `chamelio/utils.h`

## Verify pkg-config

```bash
pkg-config --cflags --libs cham
pkg-config --cflags --libs cham_udp
pkg-config --cflags --libs cham_tcp
```

If pkg-config cannot find the libraries, check that `/usr/local/lib/pkgconfig`
is in `PKG_CONFIG_PATH`:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

## Run Tests

Some tests exercise setup that may require privileges. Run the suite with:

```bash
meson test -C build -v
```

Run one test by name:

```bash
meson test -C build queue-test -v
```
