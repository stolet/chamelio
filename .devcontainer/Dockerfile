ARG VARIANT=ubuntu-24.04
FROM mcr.microsoft.com/devcontainers/base:${VARIANT}

# need to set the workdir as bpftime o/w the build fails (cmake cmd below)

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
    libelf1 libelf-dev zlib1g-dev \
    make cmake git libboost-all-dev pkg-config \
    binutils-dev libyaml-cpp-dev ca-certificates \
    clang-18 llvm-18 llvm-18-dev \
    libzstd-dev \
    python3-pyelftools gcc g++ \
    libbpf-dev \
    build-essential \
    meson ninja-build libnuma-dev \
    && rm -rf /var/lib/apt/lists/* /var/cache/apt/archives/*

#so that meson finds llvm dependency 
ENV LLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
ENV PATH="/usr/lib/llvm-18/bin:${PATH}"
ENV LD_LIBRARY_PATH="/usr/lib/llvm-18/lib:${LD_LIBRARY_PATH}"


WORKDIR /workspaces/programs/

# Installing DPDK
RUN wget https://fast.dpdk.org/rel/dpdk-25.07.tar.xz && \
    tar xJf dpdk-25.07.tar.xz

RUN cd dpdk-25.07 && \
    meson setup build && \
    ninja -C build && \
    ninja -C build install && \
    ldconfig

# Installing llvmbpf
RUN git clone https://github.com/eunomia-bpf/llvmbpf.git && \
    cd llvmbpf && \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release; \
    cmake --build build -j"$(nproc)" --parallel && \
    install -Dm644 libllvmbpf_vm.a /usr/local/lib/libllvmbpf.a && \
    install -Dm644 build/_deps/spdlog-build/libspdlog.a /usr/local/lib/libspdlog.a; \
    install -d /usr/local/include/llvmbpf && \
    cp -r include/* /usr/local/include/llvmbpf/ && \
    install -Dm644 src/ebpf_inst.h /usr/local/include/llvmbpf/ebpf_inst.h && \
    ldconfig

#Installing bpftime and bpftime-verifier
RUN git clone --recurse-submodules https://github.com/eunomia-bpf/bpftime.git && \
    cd bpftime/bpftime-verifier && \
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-include cstdint" && \
    cmake --build build -j"$(nproc)" --parallel --verbose && \
    install -Dm644 build/libbpftime-verifier.a /usr/local/lib/libbpftime-verifier.a && \
    install -Dm644 build/ebpf-verifier/libebpfverifier.a /usr/local/lib/libebpfverifier.a && \
    mkdir -p /usr/local/include/bpftime-verifier && \
    cp -r include/* /usr/local/include/bpftime-verifier/ && \
    mkdir -p /usr/local/include/prevail && \
    cp -r ebpf-verifier/src/* /usr/local/include/prevail/ && \
    ldconfig

