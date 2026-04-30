# Debug TCP and UDP State

TCP and UDP both publish a small state descriptor under `/run/chamelio`.
The matching `statetool` binary reads that descriptor, uses
`process_vm_readv()` to inspect the live slow-path process, and prints protocol
maps and socket state.

This is a shared workflow for both protocols. Use the TCP statetool for a
running TCP slow path and the UDP statetool for a running UDP slow path.

## Build the Tools

```bash
cd /workspaces/chamelio
ninja -C build protos/tcp/statetool protos/udp/statetool
```

The tools are built at:

```text
build/protos/tcp/statetool
build/protos/udp/statetool
```

## Start Chamelio and a Slow Path

Start Chamelio first, then start one protocol slow path.

UDP:

```bash
sudo ./build/protos/udp/slow/udp_slow \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

TCP:

```bash
sudo ./build/protos/tcp/slow/tcp_slow \
  --rxbuf-sz=32768 \
  --txbuf-sz=32768 \
  --appq-len=128 \
  --bumpq-len=16384 \
  --ctlq-len=1024
```

The slow path writes one state file after initialization:

```text
/run/chamelio/udp_statetool
/run/chamelio/tcp_statetool
```

## Run statetool

Run the tool in the same machine, VM, container, and PID namespace as the slow
path. Use `sudo` if the kernel denies `process_vm_readv()`.

UDP:

```bash
sudo ./build/protos/udp/statetool
```

TCP:

```bash
sudo ./build/protos/tcp/statetool
```

You can also pass an explicit state file path:

```bash
sudo ./build/protos/udp/statetool /run/chamelio/udp_statetool
sudo ./build/protos/tcp/statetool /run/chamelio/tcp_statetool
```

## What to Look For

Use the output to confirm:

- the slow-path PID is the process you expect;
- `proto shm_base`, `shm_size`, `local_ip`, and `n_fp_cores` look correct;
- maps have expected element counts, element sizes, and offsets;
- socket tables contain the sockets your application created;
- UDP port maps contain bound ports;
- TCP listener ports, bound ports, flow buckets, and control queue IDs match
  the connection state you expect.

For UDP, useful fields include socket `local_ip`, `local_port`, RX/TX buffer
lengths, availability, and port map entries.

For TCP, useful fields include socket state, local/remote addresses, listener
metadata, flow buckets, retransmission metadata, and control queue IDs.

## Common Failures

`bad UDP state` or `bad TCP state` means the state file is stale or does not
belong to the matching protocol. Restart the slow path and run the matching
tool.

`ABI mismatch` means the statetool and slow path were built from different
headers. Rebuild both:

```bash
ninja -C build protos/tcp/statetool protos/udp/statetool
ninja -C build protos/tcp/slow/tcp_slow protos/udp/slow/udp_slow
```

`failed to read ... Operation not permitted` means the tool cannot inspect the
slow-path process. Run it as root or in the same container/VM namespace as the
slow path.

Missing state files mean the slow path did not finish initialization or could
not write to `/run/chamelio`.
