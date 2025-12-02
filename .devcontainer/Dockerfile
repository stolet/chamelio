# Debian 13
FROM debian:13-slim

# Avoid interactive tz/locale prompts during install
ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Etc/UTC

# Update apt and install basic packages
RUN apt-get -y update
RUN apt-get -y install build-essential wget xz-utils git vim gdb clang linux-perf

# Install meson and ninja
RUN apt-get -y install meson ninja-build python3-pip python3-pyelftools

# Install packages necessary for DPDK
RUN apt-get -y install libibverbs-dev librdmacm-dev \
    rdma-core ibverbs-utils pkgconf libnuma-dev

# Get and extract DPDK tar ball
WORKDIR /workspaces/programs
RUN wget https://fast.dpdk.org/rel/dpdk-25.07.tar.xz && \
    tar xJf dpdk-25.07.tar.xz

# Compile DPDK
WORKDIR /workspaces/programs/dpdk-25.07
RUN meson setup build
RUN ninja -C build

# Install DPDK libraries
RUN ninja -C build install
RUN ldconfig

# Install llvmbpf dependencies
WORKDIR /workspaces/programs
RUN apt-get -y install llvm-dev libzstd-dev cmake libspdlog-dev libfmt-dev libbpf-dev

# Install llvmbpf
RUN git clone https://github.com/eunomia-bpf/llvmbpf.git
WORKDIR /workspaces/programs/llvmbpf
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build --target all --parallel

# Install the public header for llvmbpf
# Chamelio calls llvmbpf.hpp and it also includes ebpf_inst.h
# so we have to add these to /usr/local/include so that Chamelio can find them
RUN install -d /usr/local/include/llvmbpf
RUN install -m 0644 include/llvmbpf.hpp /usr/local/include/llvmbpf/llvmbpf.hpp
RUN install -m 0644 src/ebpf_inst.h /usr/local/include/llvmbpf/ebpf_inst.h

# Install the static library for llvmbpf
# We move libllvmbpf library and its spdlog dependency to /usr/local/lib
# so that Chamelio can find it
RUN install -m 0644 libllvmbpf_vm.a /usr/local/lib/libllvmbpf_vm.a
RUN install -m 0644 build/_deps/spdlog-build/libspdlog.a /usr/local/lib/libspdlog.a

# Install ecc tool to compile ebpf programs
WORKDIR /workspaces/chamelio/tests
RUN wget https://github.com/eunomia-bpf/eunomia-bpf/releases/latest/download/ecc && chmod +x ./ecc
RUN install -m 0755 ecc /usr/local/bin/ecc