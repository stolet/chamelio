# Network Virtualization

Network virtualization lets applications inside VMs use inner IP addresses
while Chamelio transmits packets over host outer IP addresses.

## When It Is Used

Use network virtualization when:

- applications run in QEMU guests;
- the guest application IP is different from the host Chamelio IP;
- packets need to be mapped between guest identities and physical host
  addresses.

Enable it with:

```bash
--virt-gre --virt-path=chamelio/netvirt/netvirt_conf.csv
```

## Tables

Chamelio parses a CSV and builds two lookup tables:

- GRE key plus inner IP to network virtualization entry;
- guest ID plus outer IP to network virtualization entry.

These tables are initialized when Chamelio starts and are read by the fast path
after initialization.

## Inner and Outer Addresses

The inner IP is the address seen by the application in the VM. The outer IP is
the address used by Chamelio on the physical network.

For example, a server VM might bind to `10.0.0.1`, while the host Chamelio
process sends and receives on `192.168.10.14`.

## GRE Key

The GRE key identifies virtualized traffic. The sample config uses key `0` for
all entries, which is enough for simple single-tenant experiments. Use distinct
keys when a deployment needs to separate virtual networks.

See [Network virtualization config](../../reference/netvirt-config/README.md)
for the CSV format.
