# Protocol Implementations

`protos/` contains Chamelio protocol implementations. A protocol normally has
four pieces:

- `include/`: protocol data structures shared by slow path, fast path, and
  application library.
- `slow/`: the slow-path daemon that registers the protocol with Chamelio,
  creates maps and queues, handles control-plane work, and serves applications.
- `fast/`: eBPF code run by the Chamelio fast path.
- `lib/`: the application-facing library used by programs.

The repository currently includes:

- [UDP](udp/README.md)
- [TCP](tcp/README.md)

For a protocol development workflow, see
[Add a new protocol](../docs/how-to/add-a-new-protocol/README.md).
