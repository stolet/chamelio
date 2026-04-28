# Network Virtualization Config

Chamelio can use GRE-based network virtualization for VM deployments. Enable it
with:

```bash
--virt-gre --virt-path=<path>
```

The default source example is `chamelio/netvirt/netvirt_conf.csv`.

## Format

The CSV columns are:

```text
GUEST_ID, GRE_KEY, OUTER_IP, INNER_IP
```

Example:

```text
GUEST_ID, GRE_KEY, OUTER_IP, INNER_IP
0, 0, 192.168.10.14, 10.0.0.1
1, 0, 192.168.10.14, 10.0.0.2
0, 0, 192.168.10.13, 10.0.0.60
1, 0, 192.168.10.13, 10.0.0.61
```

## Fields

| Field | Meaning |
| --- | --- |
| `GUEST_ID` | Chamelio guest ID assigned to a VM or guest context. |
| `GRE_KEY` | GRE key used for encapsulated traffic. |
| `OUTER_IP` | Chamelio host IP on the physical network. |
| `INNER_IP` | Guest/application IP visible inside the virtualized network. |

## Lookup Model

Chamelio builds two tables from the CSV:

- an inner table indexed by GRE key and inner IP;
- a guest table indexed by guest ID and outer IP.

Applications in VMs bind to and connect to inner IPs. Chamelio uses the table
to map those inner addresses to the outer Chamelio host addresses used on the
physical network.
