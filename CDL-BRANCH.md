# The `cdl/main` branch

**If you arrived here from a binary or a source offer, this is the branch that
binary was built from.**

This is a mirror of [Genode](https://genode.org) — the upstream is
[codeberg.org/genodelabs/genode](https://codeberg.org/genodelabs/genode) —
with **nineteen changes** by **Carrier Detect Labs** on `cdl/main`, plus the
commits that maintain this file and change no code.

`main` and `staging` are upstream, untouched. The base of this branch is
`492a510242`, **tag `26.05`**, so this is release 26.05 plus the changes below
and nothing else.

Licence is unchanged: **AGPLv3 with Genode's Section 7 linking exception**, as
upstream.

## Why this fork exists at all

Not to propose anything. It exists so that a binary we publish can be
accompanied by the complete and corresponding source **we host ourselves**,
rather than a patch series plus "clone upstream yourself", which depends on
somebody else's server still offering the same commit.

**These changes are not submitted and should not be: Genode Labs do not accept
LLM-generated patches.** Several are genuine framework defects and the reports
were written up, but they are not filed here and this branch is not a pull
request. If you hit one of them, the place to raise it is
[upstream](https://codeberg.org/genodelabs/genode), described in your own
words.

## Every change on the branch

They come from porting the [Hercules](https://github.com/SDL-Hercules-390/hyperion)
IBM mainframe emulator to Genode, where each emulated machine is a separate
component. Oldest first, as `git log --reverse 492a510242..cdl/main` lists
them. **The count has been wrong here before**; that command is the authority
and this list is checked against it, not against memory.

**FOUR CODE COMMITS ARE DELIBERATELY ABSENT FROM THE TABLE**, which is why
that command reports twenty-three and the table has nineteen rows. They are
`5c57cbded2`, `e56f190be8` and `25140aad41`, three changes to `tcp_terminal`
about a client's half-close displacing or misdelivering a connection, and
`22ad290705`, which REVERTS all three because they broke the test they were
meant to serve. Their net effect on the tree is nothing, so they are not
changes this branch carries -- but they are in the history, a reader counting
commits will find them, and a list that silently omitted four would be the
kind of discrepancy the sentence above exists to prevent. The defect they
addressed was fixed in the client instead.

| | commit | what it is |
|---|---|---|
| 1 | `44250b4d63` | lwip: narrow the ephemeral TCP port range, clear of `nic_router`'s NAT pool |
| 2 | `1ac58a22f4` | nvme: size the I/O queue allocator to the arrays it indexes |
| 3 | `69fc227759` | vfs/terminal: `read_ready()` must ask the terminal, not just the staging buffer |
| 4 | `609cee286c` | lwip: move that narrowed range clear of the emulated controller's port realm |
| 5 | `3afb045344` | dde_ipxe: drive the Intel I226 by backporting four things from upstream iPXE |
| 6 | `745123905c` | dde_ipxe: carry the whole I225/I226 family, not just the "V" variants |
| 7 | `1c46f65f47` | pci_decode: report PCIe Precision Time Measurement, and enable it on request |
| 8 | `73f8747d35` | pci_decode: prove a PTM control write reaches the device before trusting it |
| 9 | `f4160291a1` | pci_decode: make the config ROM optional, and notice init's config |
| 10 | `5b3710542e` | pci_decode: read PTM enablement from the register, not from our own write |
| 11 | `6e3724030a` | nvme: the completion-queue count was the submission count printed twice |
| 12 | `51485c9ffd` | libc: warn once that `sched_yield` is unimplemented, not once per call |
| 13 | `51ac225906` | nvme: report the granted I/O queue count, and make the request configurable |
| 14 | `5def3bbdca` | libc: count what the monitor queue was doing when a job was executed |
| 15 | `06c7b04674` | libc: say that the monitor-queue pass count is the one counter not reset |
| 16 | `e58e23782f` | libc: gate the monitor-queue timing bins on a config attribute, default off |
| 17 | `ea40fd42b5` | lwip: make the narrowed port range the only copy of the band |
| 18 | `fd974d6775` | lwip: the narrowed range's seed could start one past its end |
| 19 | `6b9d2eafea` | trace_subject_reporter: report when the subject list did not fit, and make the buffer configurable |
| — | `5c57cbded2` | tcp_terminal: a half-close from the client is not a close — **reverted** |
| — | `e56f190be8` | tcp_terminal: a new connection displaces the old one — **reverted** |
| — | `25140aad41` | tcp_terminal: a comment correction to the above — **reverted** |
| — | `22ad290705` | the revert of all three; `tcp_terminal` is 26.05's, byte for byte |

The four `tcp_terminal` rows carry no number because they cancel out: the
file they touch is the release's, unchanged. They are listed because a reader
of the log will see them, and the last one's message is the account of why.

Three of them have the long write-ups they were given when this file only
described three commits; the rest are summarised below. **Every commit message
carries its own full account**, including the measurements and the approaches
that were tried and abandoned, and that is the place to read rather than this
summary.

### `1ac58a22f4` — nvme: size the I/O queue allocator to the arrays it indexes

A framework defect, and the one most likely to matter to someone else. The NVMe
driver's I/O queue id allocator was a `Bit_allocator<MAX_IO_ENTRIES>` — 512 bits
— but the ids it returns index `_sq[]`/`_cq[]`/`_dbl[]`, which hold
`MAX_IO_QUEUES + 1` entries. Past `MAX_IO_QUEUES` sessions it hands out ids
beyond the end of those arrays and `setup_io()` indexes out of bounds.
Reachable only past 128 concurrent Block sessions, which is why it has not
bitten. Also stops a rejected submit counting as in flight, and makes the
unreachable arm of the acceptance switch return `REJECTED` rather than falling
through while reporting `ACCEPTED`.

### `69fc227759` — vfs/terminal: `read_ready()` must ask the terminal

A framework defect affecting any user of the VFS terminal plugin that moves more
than 4000 bytes in one go. Data reaches the plugin's staging buffer either in
`read()` or from the read-available signal handler; the buffer holds 4000 bytes,
so a peer writing more fills it and the remainder stays in the Terminal session.
`read_ready()` answered from the buffer alone, so `select()` and `poll()`
reported nothing to read while the session held thousands of bytes — and the
handler would not fire again until the peer wrote *more*, which a peer waiting
for a reply never does. A lost wakeup that presents as a dead link.

Found with 7754 bytes against the 4000-byte buffer; every smaller message had
gone through.

### `44250b4d63` — lwip: narrow the ephemeral TCP port range

Not a defect — a deployment choice, and arguably the wrong thing for anyone
else to copy blindly. lwIP's default ephemeral range is IANA's whole dynamic
range, 49152–65535, which is exactly the range `nic_router`'s NAT allocator
owns (`port_allocator.h`: `FIRST_PORT = 49152, NR_OF_PORTS = 16384`). A
`tcp-forward` rule for a port inside it collides with the allocator, and the
failure is silent: the router starts, reports its domains, and then never
grants the client its Nic session.

Narrowing the range is ordinary practice for a host behind NAT that must accept
inbound connections on ports chosen by the stack — here, passive-mode FTP from
an emulated mainframe. On a host with a routable address it is unnecessary.

The collision between lwIP's default and `nic_router`'s pool may still be worth
upstream's attention, since the way it fails gives no clue.

### `609cee286c` — lwip: move the narrowed range clear of a reserved band

A follow-up to the commit above and the same kind of change: a deployment
choice, not a defect. The band first chosen, 20480–20495, sits in the middle
of the range this deployment reserves for an emulated communications
controller, so it moves to 11600–11615.

The two numbers used to live in three places — here and as literals in two of
the consuming project's run scripts, which were not derived from this file —
and a disagreement fails silently and one-sidedly: lwIP picks a passive-mode
data port the router does not forward. **Since 2026-11-06 the macros below are
the only copy**: those run scripts read `TCP_LOCAL_PORT_RANGE_START`/`_END` out
of this header at build time and refuse to build if they cannot, so the two
`#define` lines are now load-bearing for a consumer outside this tree. The
trailing decimal comment on each is checked against the macro value, since the
comment is what the consuming prose quotes.

### `ea40fd42b5` — lwip: make the narrowed port range the only copy of the band

**Comment and documentation only; the macros and the band are unchanged, and
nothing compiles differently.** It is numbered as a change rather than as a
file-maintenance commit because it edits a source file, and it edits this file
in the same commit because the invariant is one sentence in both places.

What it records is that the two `#define`s above are now load-bearing outside
this tree: the consuming project's two run scripts read them at build time to
generate one `nic_router` `tcp-forward` rule per port in the band, instead of
repeating the numbers. Their build also checks each macro's trailing decimal
comment against the macro value, which is why that comment is not decoration
— it is what the consuming prose quotes, so a value changed without its
comment refuses that build rather than half-landing.

### `fd974d6775` — lwip: the narrowed range's seed could start one past its end

**The first real defect in this group**, as opposed to a deployment choice,
and it was in the fix for the deployment choice.

`TCP_ENSURE_LOCAL_PORT_RANGE` is overridden here because upstream's default is
a mask-and-add that assumes the range runs to `0xffff`. The replacement was a
modulo over `(END - START + 1)`, which seeds `tcp_init`'s `tcp_port` anywhere
in `[START, END]` **inclusive** — and `tcp_new_port` increments before it
compares, so `END` is the one value from which its `== END` wrap test cannot
fire. From there the counter walks past the band and never returns. With the
range at 11600–11615 that is one seed in sixteen, and `LWIP_RAND` here is
`genode_rand()`, PCG32 seeded from `Trace::timestamp()`, so it is a fresh draw
every boot rather than a fixed outcome: roughly 6% of boots allocated ports
nobody had forwarded, and a port outside the band fails silently — an FTP
control connection that works and a data connection that hangs.

The modulo is now over `(END - START)`, seeding into `[START, END-1]`, which
is exactly the set `tcp_new_port` can return. So `END` is forwarded and never
allocated and a sixteen-port band has fifteen usable ports; that is documented
rather than fixed, because the spare rule costs nothing.

**The comment in that file had ruled this out** — "the modulo below cannot do
that" — and ruling out a failure mode in a comment is a claim, not a fact.
It is replaced with what `tcp_new_port` does.

### `3afb045344`, `745123905c` — dde_ipxe: the Intel I225/I226 family

Support for the Intel I226-V and I225-V gigabit controllers in the iPXE NIC
driver, so Genode can use the Ethernet on hardware newer than the pinned iPXE
revision knows about. **A backport rather than a port bump**, because upstream
iPXE drives these parts with its ordinary `intel.c` and an existing flag, so
what the pinned revision lacks is small and self-contained: two
packet-buffer-size registers with the I210 power-up defaults, and the device
ids. Rebasing the seven patches the port already carries — four of which touch
`intel.c` — is the larger risk.

The second commit adds the "LM" device ids, which the first deliberately left
out. The omission had been described as a second guard behind the platform
driver's policy, on a machine whose LM port carries the management engine that
is its only console. It was not a guard: `dde_ipxe` is handed exactly one
device, so listing an id cannot reach a device the policy does not grant. A
guard that does not guard is worse than none, because it is believed.

### `1c46f65f47`, `73f8747d35`, `f4160291a1`, `5b3710542e` — pci_decode: PCIe Precision Time Measurement

Groundwork for disciplining an emulated mainframe's time-of-day clock from a
network time source. The capability gets a definition in the shared PCI
header, riding the extended-capability walk already there; the bus-scanning
component reports what it finds for every device that has one, which is a pure
read; and it can be asked to enable it.

The other three are what it took to make that report trustworthy, and each is
a lesson rather than a feature:

* **A readback is not a test.** Firmware on the development machine enables
  PTM during power-on self test, so the register already held the value the
  enable pass would write: writing it and reading it back proves nothing,
  since a write that never left the component gives an identical log. With
  `verify` set the register is cleared and read back first, and says so as an
  error if that does not read zero.
* **A component with no inline `config` node still gets a ROM.** Adding
  configuration to a component that never had any was expected to kill every
  scenario that starts it. Measured instead: the request succeeds, reaches
  core under the usual route to the parent, and is answered with the boot
  module named `config` — **init's own config for the whole scenario**. The
  component parsed the wrong document and found a root node of the wrong name.
* **A dry run must predict what the real run does.** A bridge's recorded
  "PTM enabled" state was set only on the path that writes the register, so a
  dry run left it false and every requester below reported its responder
  disabled — on the line after printing that responder's `ctrl=0x00000003`,
  Enable plainly set. The report contradicted itself and predicted the
  opposite of what a write run then did.

### `6e3724030a`, `51ac225906` — nvme: the I/O queue count

The driver creates one I/O queue per Block session, so the number of queues a
controller grants is a hard ceiling on how many clients it can serve. That
number was reachable only as a log line; it is now in the controller report
(`<io_queues granted="16" completion="16" requested="128"/>`), so a component
sizing a storage layout against it can read it, and the requested count is
configurable.

The other commit fixes the line that reported it:

    uint16_t const ncqa = 1u + (dw0 & 0xffff0000u >> 16);

`>>` binds tighter than `&`, so that expression is `dw0 & 0xffffu` — the
submission count, printed under the completion label. Every controller tried
allocates the two equally, so the wrong answer was always the right one, which
is why it survived.

### `51485c9ffd` — libc: `sched_yield` warns once, not once per call

Same answer to every caller as the dummy it replaces, `ENOSYS` and `-1`, so
nothing can behave differently. The reason is log volume: one partition of one
boot produced 14,542 warnings in the nine seconds between the load command and
the operating system's first console prompt — about a megabyte of log, which
buries every other line in a capture, and worse on a machine whose only
console is a serial-over-LAN session.

### `5def3bbdca`, `06c7b04674`, `e58e23782f` — libc: the monitor queue's instrument, and its gate

**This is the one change on the branch that is an instrument rather than a fix,
and the only one whose cost you could notice.** It exists because of a
measurement that nothing in a caller could make.

Genode's libc answers `poll()` by enqueueing a job for the component's
entrypoint and waiting — before it consults the timeout, so even a zero-timeout
`poll()` is an entrypoint round trip. In the system this branch was made for,
one such call read 40 microseconds from one thread and 9,010 from another, in
the same component and the same second. Every explanation available from a
caller had been excluded, so `Libc::Monitor::Pool` was made to count, per
completed synchronous job, how many examination PASSES elapsed between its
enqueue and its execution and how many other jobs the entrypoint got through in
that pass first. The answer was that **nothing queues** — not one job in a
92-second window waited a second pass — and that the entrypoint's pass rate had
fallen from 4,029 a second to 223, because a thread that never yields was
holding its core at equal priority. The instrument is what distinguished "the
round trip is expensive" from "the round trip is behind other work", and the
second reading was the one this project had wrongly carried for days.

**Why it is off by default, as of `e58e23782f`.** Filling the per-job timing
bins costs two `Genode::Trace::timestamp()` reads per job, and that primitive
serialises with `cpuid`. In a library every libc component links, that is a
cost imposed on code that never asked for it, so it now needs the
`monitor_stats` attribute on a component's own `<libc>` config node and is
FALSE without it. The flag is read once during `Kernel` construction, before
any pthread exists to enqueue a job.

**What still counts when it is off**, deliberately: `monitor_passes`,
`monitor_examined` and `monitor_completed`. Those are plain increments, and the
pass rate alone answers "is the entrypoint running" — which was the whole
finding. `libc_monitor_stats_enabled` says which state a reader is looking at,
because an instrument that is off and a queue with no traffic return identical
empty bins.

**If you are reading this branch to decide whether to carry the change:** the
two counting commits are additive and the third makes them inert unless asked
for, so taking all three costs a configuration attribute you never set. Taking
none of them costs nothing either — no other change on this branch depends on
them.

### `5c57cbded2`, `e56f190be8`, reverted by `22ad290705` — tcp_terminal

A defect in common code, affecting any client of this server that is fed from
a pipe. On a zero-length read the server closed the socket, and a zero-length
read is not the peer going away: it is the peer having **stopped sending**,
which a request/response client does the moment its input ends, before waiting
to be answered on the same connection. The reply then went to file descriptor
-1, with three "no plugin found" errors that point at the libc socket plugin
rather than at the close that caused them. A second defect in the same five
lines: `read()`'s `ssize_t` result was assigned to a `size_t`, and only `== 0`
was handled, so any error on an open socket became `SIZE_MAX` and the client
read stale buffer for ever.

**ALL OF THIS IS REVERTED, and the reason is the useful part.** The fix kept
the descriptor open for writing, ended only the read side, and omitted an EOF
socket from the `select()` read set.

**The second commit was required by the first, and the obvious form of it
was wrong.**
The listen socket was watched only while no connection existed, so the close
on a zero read was also what returned the session to listening: removing it
left a session that answers exactly one client and is then deaf. Re-arming the
listen socket when the peer stops sending looks like the repair and fails,
because **this stack cannot be relied upon to notice the peer leaving**: a FIN
arriving while the read buffer is still occupied — exactly what a half-closing
client produces — is never reported by a later `select()`. The session now
watched the listen socket unconditionally so that a **new connection displaced
the one in place** -- a deliberate behaviour change against upstream, where a
second client waits in the backlog until the first closes.

**That is what broke it.** A connection queued while the server was absent is
accepted later and takes the port from a live client, and the server's next
reply goes to the hijacker. Measured against a management-plane gate that
destroys and recreates its server mid-run: the gate failed three times with
these commits and passes with the release's file. Upstream's rule -- accept
only while the descriptor is -1 -- makes a late arrival wait harmlessly in the
backlog, and there is no way to tell that stale connection from a real one,
because the thing to tell apart is exactly whether a peer that has stopped
talking is still there.

So the fix belongs in the client, which must not half-close while it is still
waiting to be answered, and that is where it now lives. One narrow server-side
defect in the same lines remains worth reporting upstream on its own: `read()`'s
`ssize_t` result is assigned to a `size_t` and only `== 0` is handled, so an
error on an open socket becomes `SIZE_MAX` and the client reads stale buffer
for ever.
