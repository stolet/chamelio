# Incremental Homa plan

Build an eTRAN-era Homa implementation in eight increments: protocol/library
integration, small RPCs, completion and reclamation, segmentation/reassembly,
receiver grants, latency scheduling, loss recovery, and multicore/VM validation.
The first increments deliberately assume a lossless network. The complete target
includes all eight; small-message echo alone does not establish Homa performance.

This is a feature and component specification. API signatures, state layouts,
data structures, algorithms for lookup, and tuning are left for implementation
design. No protocol code is introduced by this document.

## Baseline and scope

The primary comparison is eTRAN Homa at
[`f26ef186bde0f9b3b899712e44112de47b7d5a65`](https://github.com/eTran-NSDI25/eTran/tree/f26ef186bde0f9b3b899712e44112de47b7d5a65),
the public NSDI 2025 artifact. Chamelio was inspected at
`42275f42ca181e286f044a89168cfe584feb6de5`.
The [eTRAN paper, sections 4.2 and 6.1](https://www.usenix.org/system/files/nsdi25-chen-zhongjie.pdf)
provides the architectural and evaluation baseline; the checked-in code determines
which features are actually present.

Use the [original Homa paper](https://arxiv.org/pdf/1803.09615) for the protocol's
rationale: connectionless request/response messages, immediate unscheduled data,
receiver grants, priorities, and overcommitment. Use the
[Homa/Linux paper, sections 4.3–4.5](https://www.usenix.org/system/files/atc21-ousterhout.pdf)
for the implementation lessons about pacing, concurrency, fairness, and deferred
reclamation. The original paper's at-least-once/re-execution behavior is superseded
by the response-retention and ACK lifecycle in the
[Linux baseline protocol description](https://github.com/PlatformLab/HomaModule/blob/8321cdecc62e25a6b66a9ff0a4cb16227c61e277/protocol.md).
That baseline, commit `8321cde`, is also the Linux comparator named by eTRAN.

Start with IPv4, RPC requests and replies, a 1500-byte physical MTU, and messages
up to eTRAN's 1,000,000-byte limit. Use eTRAN's packet formats as the reference
for data-path validation; its
[wire definitions](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/common/tran_def/homa.h)
use experimental IP protocol number 253. Full interoperability with today's
upstream Homa is outside this target: upstream has since changed both the IP
protocol number and the startup of scheduled messages, as recorded in its
[change history](https://github.com/PlatformLab/HomaModule/blob/8db9f5e6654de0e3deab40829a3264ef1eae98cc/README.md).

The public artifact needs qualification as a feature checklist.
Its [ingress control dispatch](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/eBPF/homa/main.c#L416)
handles RESEND, UNKNOWN, GRANT, and BUSY, but has no receive cases for NEED_ACK,
ACK, or CUTOFFS. Its
[timer code](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/homa.cc#L365)
does send NEED_ACK, and its DATA path implements piggybacked completion ACKs.
Therefore, complete idle cleanup cannot be assumed from the paper or packet enum.
The plan includes the small ACK fallback needed for correct sustained operation;
it does not reproduce an incomplete cleanup path. This is a source inspection
finding, not a claim from running eTRAN.

It also defines workload cutoff tables, but
[`get_prio()`](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/eBPF/homa/homa.h#L48)
never advances its cutoff index from zero. With the checked-in tables and valid
message sizes, that function selects the highest priority. Thus multiple
size-based unscheduled priority classes are not established by this artifact.
Start with its effective priority configuration; add finer unscheduled classes
only if measurements show they are needed for the latency target.

## Components and placement

Follow Chamelio's existing
[protocol model](protocol-model/README.md) and
[new-protocol workflow](../how-to/add-a-new-protocol/README.md).
Use UDP for library attachment, contexts, socket/bind handling, and queues; use
TCP for scheduler integration, control queues, retained transmit data, and timers.
Homa needs message/RPC state in place of TCP connection and byte-stream state.

| Component | Responsibility |
| --- | --- |
| Homa application library | Thread contexts, endpoints, sending requests, receiving complete requests/replies, replying by RPC handle, polling completions/errors, and releasing message storage. |
| Homa shared state | Endpoint/port lookup, active RPCs, message storage, receive progress, send eligibility, and scheduling state in protocol-local shared memory. |
| Homa fast path | Create incoming RPC state; process DATA, GRANT, and normal completion ACKs; move payload; generate grants; select and pace transmissions. |
| Homa slow path | Register the protocol, create endpoints and shared resources, manage configuration, detect timeouts, and initiate recovery/abort and exceptional cleanup. |
| Chamelio infrastructure | Admit the protocol, carry its IPv4 packets correctly, schedule pending work, steer receive traffic, and preserve priorities through virtualization. |

Ordinary RPC creation and grants must stay in the fast path. eTRAN makes this
same choice because grants sit directly on the transmission critical path; see
its [Homa data path](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/eBPF/homa/main.c)
and [RPC processing](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/eBPF/homa/rpc.h).
Incoming requests must not require a slow-path allocation round trip.

## Build and communication environment

Development belongs on the `homa` branch in
`/local/mstolet/projects/chamelio` on **swsnetlab03**. The second Chamelio
instance runs on **swsnetlab04**. All Chamelio and benchmark builds and
executables must run inside their appropriate containers on each machine.

The existing setup is the `chamelio` Compose service in
`/local/mstolet/projects/sigcomm24-artifact/dockerfiles/docker-compose.yml`.
It uses the existing `dockerfiles-chamelio:latest` image and names its container
`chamelio-baseline`. Both repositories are mounted inside this container:

- Chamelio: `/workspaces/chamelio`.
- Benchmarks: `/workspaces/benchmarks`, backed by
  `/local/mstolet/projects/chamelio-benchmarks/benchmarks`.

Use that existing image and Compose configuration without rebuilding or pulling
an image. Neither host had an existing container at inspection time. Do not
install host packages, run host builds, or change NIC bindings, hugepage settings,
or other machine configuration as part of a build check.

Once the container is running, the existing build command is:

```bash
docker exec -w /workspaces/chamelio chamelio-baseline ninja -C build
```

When the new Homa library must be exposed to the external benchmark build, run
`ninja -C build install` and `ldconfig` inside that same container. Installation
is into the container's `/usr/local`, not the host. Configure/reconfigure and
build the benchmark from `/workspaces/benchmarks` inside the container too.
Reuse the available dependencies; report any missing dependency instead of
installing additional software or rebuilding the image automatically.

The current swsnetlab04 Chamelio checkout is on `energy`. Before communication
checks, provide a separate checkout/worktree containing the matching Homa
changes and select it through the existing `ARTIFACT_CHAMELIO_DIR` Compose
mount override. Preserve the existing remote checkout and its branch. Build
the matching stack, Homa library, and echo applications inside each host's
Chamelio container.

Start Chamelio, then the Homa slow path, on both machines. Run the echo server
in swsnetlab04's container and the client in swsnetlab03's container, using SSH
and `docker exec` for the remote commands. Existing experiment defaults assign
`192.168.10.13` to swsnetlab03 and `192.168.10.14` to swsnetlab04; reuse their
configured data NIC and CPU placement rather than guessing new settings.
These addresses are configuration values, not a verified communication result.

### Homa echo application

Add a `homa/` directory alongside `tcp/` and `udp/` in the benchmark repository,
with `homa_cham_server`, `homa_cham_client`, and a `homa_cham` Meson alias.
Use the existing echo programs as the model, preserving their argument style:

- Server: `<bind_ip> <port>`, with `--msize` and `--ncores`.
- Client: `<src_ip> <src_port> <rem_ip> <rem_port>`, with `--msize`,
  `--duration`, `--rate`, and `--ncores`.

The server replies with the received payload using the request's RPC handle.
The client confirms the echoed data arrives back. Reuse existing timing,
reporting, and CPU-placement utilities where needed. Begin with one application
thread, one fast-path core, and small messages; increase message size when
segmentation and grants are available. No new benchmark framework is needed.

## Ordered increments

### 1. Minimal RPC library and Chamelio integration

Add a Homa protocol registration, slow-path process, shared resource setup,
eBPF handlers, library, and build/install targets. Expose only endpoint
creation/bind/close, request/reply, completion polling, and message release.
Use one application context and one fast-path core initially. Establish bounded
RPC/message capacity and explicit buffer ownership from the start.

Reuse Chamelio's shared-memory queues and registration machinery. Provide both
programmable eBPF and handwritten fast-path handlers, following TCP and UDP,
and support the existing programmable compilation modes.

**Verification:** build the Homa targets successfully. No network handshake is
needed: a Homa endpoint can address a new peer immediately.

### 2. Single-packet requests and replies

Send an unscheduled request DATA packet, deliver the complete request, and send
a reply associated with the same RPC. Match replies to the requesting context
and distinguish simultaneous RPCs and peers. Create incoming RPC state in the
fast path from prepared shared resources. Keep ordinary payload transfer and
completion notification off the slow path.

Enable correct IPv4 transmission for Homa in the host infrastructure. Until the
next increment, this is a bounded bring-up experiment with retained server
state, rather than a sustained workload.

**Verification:** build successfully and run a single-packet request/reply
between two machines, confirming that the client receives the expected reply.

### 3. RPC completion and buffer reclamation

Use the reply as confirmation of request delivery. Retain the request until
that confirmation, and retain server response data until the client confirms
receipt. Piggyback completion ACKs on later DATA packets to the same peer.
Add standalone ACK/NEED_ACK handling so a final RPC can be reclaimed when no
further traffic arrives, including when a completion ACK is lost.
Suppress request redelivery while the corresponding server RPC is still active
or retaining its response.

Release application receive storage separately from transport retransmission
storage. Reclaim resources in bounded amounts so a completed large RPC will
not later stall packet processing. Socket close must also release active state.
The [Linux ACK handlers](https://github.com/PlatformLab/HomaModule/blob/8321cdecc62e25a6b66a9ff0a4cb16227c61e277/homa_incoming.c#L739)
provide the missing reference for the fallback.

**Verification:** build successfully and repeat the small request/reply check.

### 4. Segmentation and complete-message reassembly

Extend both requests and replies to multiple DATA packets, initially limited to
the unscheduled-byte allowance. Preserve message boundaries and place segments
by offset. Accept reordering, suppress duplicate segments, and notify the
application only once all bytes have arrived. Multiple RPCs must progress
independently; an incomplete message must not block unrelated complete messages.

Apply bounded message admission and backpressure when storage or RPC capacity
is exhausted. Avoid allocating new shared-memory objects through the slow path
for every message. eTRAN's
[library](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/lib/eTran_rpc.cc)
is the reference for message delivery and segmentation behavior, rather than
the byte-stream interface of Chamelio TCP.

**Verification:** build successfully and exchange a multipacket request/reply
with the expected payload, within the unscheduled allowance.

### 5. Receiver grants for larger messages

Keep the initial unscheduled portion, then require receiver GRANTs for further
DATA. A grant authorizes transmission through a cumulative message offset.
Replenish grants as bytes arrive and process them promptly at the sender.
Implement this symmetrically for requests and replies.

Track authorized-but-not-received bytes and cap additional grants using a
receiver incoming-byte budget. Account for known unscheduled traffic too;
that budget cannot prevent initial bursts from previously unseen senders.
Grant only against available receive storage. Generate grants without requiring
outbound application data, and resume pending work when storage becomes free.

Begin with one active scheduled message to validate the mechanism. This is an
intermediate restriction, removed in the next increment. Use configurable
unscheduled/grant windows appropriate to the link and software round trip.

**Verification:** build successfully and exchange a request/reply larger than
the unscheduled allowance, exercising the grant path.

### 6. Homa scheduling, network priorities, and pacing

Complete the performance mechanisms as one milestone:

- Receiver scheduling favors messages with the fewest bytes remaining. Grant
  to several peers concurrently to keep the downlink busy, ordinarily choosing
  at most one scheduled message per peer at a time.
- Sender scheduling favors the eligible message with the fewest bytes left to
  send. A message waiting for credit must not block other eligible messages.
- Give unscheduled DATA high network priority and use receiver-selected
  priorities for scheduled DATA. Begin with the artifact's effective markings;
  finer size-based unscheduled classes are conditional on measured need.
  Configure switch priority queues and preserve markings on the actual wire.
- Pace aggregate output to limit NIC queue buildup. Short messages and urgent
  control work must make progress promptly, even during large transfers.
- Reserve a small amount of service for old messages at both grant selection
  and the sender, preventing starvation under sustained short-message load.

These mechanisms are represented in eTRAN's
[grant selection](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/eBPF/homa/main.c#L590),
[pacer](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/eBPF/homa/pacing.h),
and [configuration](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/eBPF/homa/bss_data_defs.h),
subject to the priority-selector qualification above. Their kernel-specific
data structures are not requirements for Chamelio.

**Verification:** build successfully and repeat the small and large
request/reply checks with scheduling enabled.

### 7. Loss recovery and failed RPCs

Add receiver timeout detection and RESEND requests for missing byte ranges.
Retransmit from retained data, tolerate duplicate control packets, and recover
lost grants through Homa's recovery exchange. Use BUSY to distinguish intentional
delay from lack of progress. Handle UNKNOWN, including the case where every
initial request packet was lost and the server has no RPC state.

Keep timeout detection and retry decisions in the slow path; submit recovery
work to the fast path. Do not timeout merely because an RPC is waiting for a
grant or the local sender has not yet transmitted eligible bytes. Bound retries,
report failed RPCs to applications, and reclaim associated resources.
Follow eTRAN's
[timeout/recovery split](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/homa.cc)
and the [Linux recovery handlers](https://github.com/PlatformLab/HomaModule/blob/8321cdecc62e25a6b66a9ff0a4cb16227c61e277/homa_incoming.c#L610).

**Verification:** build successfully and repeat the request/reply checks with
recovery support enabled. Fault injection is outside this verification scope.

### 8. Multicore operation, VM support, and comparison

The current increment supports up to eight independent application contexts,
each with private queues and 16 reserved client RPC slots. All contexts share
the existing 128 server RPC slots and 64 endpoints. Homa keeps all network
processing, grants, pacing, and recovery execution on fast-path core zero.
Additional Chamelio fast-path cores do not process Homa packets. Receive
steering, distributed transport ownership, and VM validation remain deferred.
No per-core budget split is needed while one core performs all network work.

Extend to multiple application contexts and fast-path cores. Provide working
receive steering for Homa and stable RPC ownership or synchronization; keep
application notifications on the correct queues. Coordinate grants and pacing
across cores so adding cores does not multiply the receiver budget or allowed
NIC backlog. Avoid a single socket-wide lock serializing all RPCs.

Exercise the existing VM registration/shared-memory path and GRE virtualization.
Account for encapsulation in payload sizing. Keep the existing shared GRE
behavior that copies inner TOS into outer TOS. Validate programmable modes
and existing performance accounting in the shared fast path.

**Verification:** build successfully and repeat the request/reply check in the
multicore and VM/GRE configurations when those environments are available.
Add more elaborate load balancing only if measured skew requires it.

#### VM integration: environment checks and validation order

Read-only inspection on 2026-10-26 established the following on both
swsnetlab03 and swsnetlab04:

- The artifact's Chamelio launch branch selects `images/base-cham.qcow2`
  and `images/seed-cham.img`. Both files exist in each host's
  `/local/mstolet/projects/sigcomm24-artifact/images` directory.
- `qemu-img info --backing-chain` reports self-contained 48 GiB qcow2 disks,
  with no backing-file dependency and no dirty/corrupt flag. This metadata
  check does not establish guest filesystem integrity or software readiness.
- Each existing `chamelio-baseline` container provides QEMU 10.0.11, KVM,
  `ivshmem-doorbell`, and the mounted artifact images. Opening `/dev/kvm`
  and querying its API succeeds inside both containers.
- The artifact defines one eight-vCPU, 8 GiB Chamelio VM per host, host-side
  Chamelio with GRE, and guest-side protocol/application containers. It maps
  client inner IP `10.0.0.60` to outer IP `192.168.10.13`, and server inner
  IP `10.0.0.1` to outer IP `192.168.10.14`.

Subsequent snapshot preflight confirmed the existing guest Docker image,
source mounts, build tools, and VFIO device/group on both hosts. An existing
UDP echo passed between the guests. The preflight procedure was:

1. Start host Chamelio in the existing containers and boot the existing
   Chamelio disks in snapshot mode, using the artifact's ivshmem launch
   branch. Keep the base disks unchanged. Use management SSH on port 2220
   of each host, independently of the GRE data path.
2. Inspect the guest before starting workloads: confirm login, Docker,
   available disk space, the Chamelio container image, and source mounts.
   Reuse the guest image if present; if its container image is missing,
   consider loading the existing host image without rebuilding or pulling.
   Stop and discuss missing prerequisites instead of installing packages.
3. Confirm ivshmem's actual PCI address and VFIO group. The current helper
   expects `0000:00:03.0` and `/dev/vfio/noiommu-0`; use explicit QEMU PCI
   placement and verify the guest binding. Limit VFIO preparation to the
   guest. Build matching sources inside the guest container.
4. Run one small existing UDP echo between the two guest containers to
   establish that ivshmem, guest containers, and GRE work independently of
   Homa. Resolve an environment failure before implementing Homa changes.

Once preflight passes, integrate Homa's guest registration and application
mapping, correct its guest inner-address handling, and account for GRE in
segmentation, reassembly, and recovery. Current registration returns the
host outer address, while Homa validates binds against that address. Also,
the 1500-byte physical MTU leaves 1392 DATA payload bytes after the existing
Homa headers and 28-byte outer IPv4/GRE overhead, rather than today's 1420.

Build inside the host and guest containers. Start the guest Homa slow paths
with `--virt`, then run short 64-byte and 1,000,000-byte payload-checked echo
checks between guest inner addresses. Reverse client/server roles and repeat
with native and eBPF host handlers. Keep one context and fast-path core zero;
leave receive steering and GENEVE out. Stop the test VMs and discard their
snapshot changes after collecting results.

The VM implementation returns the guest's inner address and usable IPv4 MTU
at protocol registration. Homa uses 1420-byte DATA segments without GRE and
1392-byte segments with GRE, consistently across transmit, reassembly,
grants, and recovery. GRE transmit preparation requests both IPv4 checksums
for Homa without requesting a transport checksum. TOS copying is unchanged.

Snapshot validation passed on both hosts with native and eBPF handlers:
64-byte and 1,000,000-byte messages in both directions, payload validation
enabled, zero RPC errors, and no unfinished RPCs. These checks establish
communication, not isolated performance results. Both guests were shut down
gracefully after each handler-mode trial.
Bare-metal eBPF regression checks also passed for both message sizes with
payload validation, zero RPC errors, and no unfinished RPCs.

## Chamelio changes that need explicit attention

These are integration requirements found in the current code, not reasons to
redesign the framework:

- [Protocol IDs](../../lib/utils/queue/include/queue_types.h),
  [bare-metal admission](../../chamelio/control/guestif.c), and
  [VM admission](../../chamelio/control/control_guest.c) currently admit TCP/UDP.
- [IPv4 transmit preparation](../../chamelio/fast/infra.c) currently configures
  checksum handling for TCP/UDP. It must correctly transmit Homa IPv4 packets.
  Preserve its existing copy of inner TOS into GRE outer TOS.
- [NIC setup](../../chamelio/nic/nic.c) requests TCP/UDP RSS and GRE steering.
  Native Homa traffic needs a verified steering path; a TCP-like header layout
  alone does not establish hardware RSS support.
- The [scheduler interface](../../lib/utils/scheduler/include/scheduler_fns.h)
  and [poll loop](../../chamelio/fast/fast_sched.c) must support pending grants
  and recovery control work as well as payload. Time eligibility, message rank,
  and aggregate pacing must all be enforced; TCP's per-flow pacing policy is
  insufficient by itself. Reuse the existing interface where practical and
  measure its cost as the active RPC count grows before replacing it.

## Verification scope

For each increment, verify that the affected targets build inside the existing
Chamelio container. From increment 2
onward, also run a simple client/server exchange and confirm that packets travel
in both directions and the client receives the expected reply. Increase message
size only as needed to exercise segmentation and grants. Use the container on
swsnetlab03 for the client and the container on swsnetlab04 for the server.

This work does not require a new test harness, fault injection, load testing,
or an eTRAN benchmark campaign. Report the build and communication checks
actually performed; if the network environment is unavailable, report that
the communication check was not run.

## Performance acceptance

With one connection we should expect p99 tail latency to be under 50 microseconds.
Ideally under 30 microseconds.

## Features outside the initial target

Omit connection setup/listen/accept, TCP-style byte streams and per-packet ACKs,
and a separate TCP-style congestion controller. Homa's grants and priorities
provide the transport control needed here.

Also omit adaptive cutoff computation and CUTOFFS exchange, special incast
startup flags, IPv6, TCP encapsulation/hijacking, current upstream START_MSG
behavior, TSO/GSO, public zero-copy APIs, kernel integration, and diagnostic
packet types such as FREEZE. Shared-memory payload transfer remains in scope.
Finer configured unscheduled priority classes can be
added if the latency measurements require them. The eTRAN artifact has
[workload cutoff tables](https://github.com/eTran-NSDI25/eTran/blob/f26ef186bde0f9b3b899712e44112de47b7d5a65/eTran/micro_kernel/eBPF/homa/bss_data_defs.h)
and no CUTOFFS receive case; its paper evaluates software segmentation without
requiring a zero-copy application API. None of the omitted features is a
prerequisite for the stated comparison.
