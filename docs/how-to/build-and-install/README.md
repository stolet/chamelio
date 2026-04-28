# Build and Install Chamelio

This guide builds the repository and installs Chamelio libraries so external
applications can use them.

## Use the Dev Container

The easiest supported environment is the repository dev container. It includes
the DPDK and libbpf dependencies expected by the Meson build.

If you use VS Code, install the Dev Containers extension, open this repository,
and run `Dev Containers: Open Folder in Container`.

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
