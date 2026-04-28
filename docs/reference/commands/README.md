# Commands

This page summarizes the command-line options exposed by the Chamelio binary
and the included protocol slow paths.

## Chamelio

Executable:

```bash
./build/chamelio/chamelio [OPTIONS] --ip-addr=ADDR[/PREFIX]
```

Common minimal run:

```bash
sudo ./build/chamelio/chamelio \
  --ip-addr=192.168.10.14/24 \
  --fp-cores-max=1 \
  --dpdk-extra="-a0000:d8:00.0" \
  --dpdk-extra="--lcores=0@1,1@3"
```

Memory options:

| Option | Default | Meaning |
| --- | ---: | --- |
| `--shm-len=LEN` | `1073741824` | Shared memory length for one guest. |
| `--shm-internal-len=LEN` | `33554432` | Internal Chamelio shared memory length. |
| `--cham-queue-len=LEN` | `16384` | Fast path/control path queue length. |
| `--agt-queue-len=LEN` | `16384` | Guest agent/Chamelio queue length. |
| `--control-txq-len=LEN` | `1024` | Control-path transmit queue elements. |
| `--control-txq-pkt-len=LEN` | `1500` | Packet bytes per control transmit queue entry. |

IP options:

| Option | Meaning |
| --- | --- |
| `--ip-addr=ADDR[/PREFIX]` | Local Chamelio IP address. |
| `--ip-route=DEST[/PREFIX],NEXTHOP` | Add a route for the Chamelio IP path. |

Fast-path options:

| Option | Default | Meaning |
| --- | ---: | --- |
| `--fp-cores-max=CORES` | `1` | Number of Chamelio fast-path cores. |
| `--fp-no-xsumoffload` | off | Disable TX checksum offload. |
| `--fp-jitcomb` | off | Enable combined infrastructure and eBPF JIT. |
| `--fp-proto=ebpf|hand` | `ebpf` | Select eBPF or handwritten fast-path mode. |

Performance isolation options:

| Option | Default | Meaning |
| --- | ---: | --- |
| `--perf-iso` | off | Enable per-guest budget enforcement. |
| `--perf-iso-cap=US` | `1000` | Budget cap in microseconds. |
| `--perf-iso-max-ins=NR` | `UINT_MAX` | Maximum accepted longest-loop instruction count. |
| `--perf-iso-boost=FLOAT` | `0.5` | Budget boost multiplier. |

Virtualization options:

| Option | Default | Meaning |
| --- | --- | --- |
| `--virt-gre` | off | Use GRE network virtualization. |
| `--virt-path=PATH` | `net_virt.csv` | Network virtualization CSV path. |

NUMA and DPDK options:

| Option | Meaning |
| --- | --- |
| `--numa-rxring=NUM` | NUMA node for DPDK RX rings. |
| `--numa-txring=NUM` | NUMA node for DPDK TX rings. |
| `--numa-mpool=NUM` | NUMA node for DPDK mbuf pool. |
| `--numa-shm=NUM` | NUMA node for guest shared memory. |
| `--numa-shm-internal=NUM` | NUMA node for internal shared memory. |
| `--dpdk-extra=ARG` | Append one argument to DPDK EAL. Repeat as needed. |

## UDP Slow Path

Executable:

```bash
./build/protos/udp/slow/udp_slow [OPTIONS]
```

Common run:

```bash
sudo ./build/protos/udp/slow/udp_slow \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

Options:

| Option | Default | Meaning |
| --- | ---: | --- |
| `--virt` | off | Run as a VM guest protocol. |
| `--rxbuf-sz=BYTES` | `32768` | Per-socket receive buffer bytes. |
| `--txbuf-sz=BYTES` | `32768` | Per-socket transmit buffer bytes. |
| `--appq-len=NELEMS` | `128` | Application/slow-path queue elements. |
| `--bumpq-len=NELEMS` | `65536` | Fast-path bump queue elements. |
| `--ctlq-len=NELEMS` | `1024` | Control queue elements. |
| `--block=NR` | none | Use UDP fast-path object for block size `64`, `128`, `256`, `512`, or `1024`. |

## TCP Slow Path

Executable:

```bash
./build/protos/tcp/slow/tcp_slow [OPTIONS]
```

Common run:

```bash
sudo ./build/protos/tcp/slow/tcp_slow \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

Memory and VM options:

| Option | Default | Meaning |
| --- | ---: | --- |
| `--virt` | off | Run as a VM guest protocol. |
| `--rxbuf-sz=BYTES` | `32768` | Per-socket receive buffer bytes. |
| `--txbuf-sz=BYTES` | `32768` | Per-socket transmit buffer bytes. |
| `--appq-len=NELEMS` | `128` | Application/slow-path queue elements. |
| `--bumpq-len=NELEMS` | `65536` | Fast-path bump queue elements. |
| `--ctlq-len=NELEMS` | `1024` | TCP control queue elements. |

Congestion-control options:

| Option | Default | Meaning |
| --- | ---: | --- |
| `--cc=const-rate|dctcp-rate` | `const-rate` | Congestion-control algorithm. |
| `--cc-rtt-init=US` | `50` | Initial RTT estimate. |
| `--cc-control-granularity=US` | `50` | Minimum delay between control-loop iterations. |
| `--cc-control-interval=NR` | `2` | Control interval in multiples of RTT. |
| `--cc-remit-ints=NR` | `4` | Control intervals without ACKs before retransmission. |
| `--cc-dctcp-weight=FLOAT` | `0.0625` | DCTCP EWMA weight. |
| `--cc-dctcp-init=KBPS` | `10000` | Initial DCTCP rate. |
| `--cc-dctcp-step=KBPS` | `10000` | DCTCP additive increase step. |
| `--cc-dctcp-mimd=FLOAT` | `1.0` | DCTCP multiplicative increase factor. |
| `--cc-dctcp-min=KBPS` | `0` | Minimum DCTCP rate. |
| `--cc-dctcp-minpkts=NR` | `50` | Minimum ACK samples before DCTCP processing. |
| `--cc-const-rate=KBPS` | `0` | Constant rate, where `0` means unlimited. |

## Echo Benchmarks

Benchmark sources live outside this repository in
`/local/mstolet/projects/chamelio-benchmarks/benchmarks`.

UDP:

```bash
udp_cham_server <bind_ip> <port> [--msize N] [--ncores N] [--buf-size N] [--cores LIST]
udp_cham_client <src_ip> <src_port> <rem_ip> <rem_port> [--msize N] [--duration S] [--rate R] [--ncores N] [--cores LIST]
```

TCP:

```bash
tcp_cham_server <bind_ip> <port> [--msize N] [--ncores N] [--buf-size N] [--mconns N]
tcp_cham_client <src_ip> <src_port> <rem_ip> <rem_port> [--msize N] [--duration S] [--rate R] [--ncores N] [--nconns N] [--mpending N] [--cwait S]
```
