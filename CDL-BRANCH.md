# The `cdl/main` branch

**If you arrived here from a binary or a source offer, this is the branch that
binary was built from.**

This is a mirror of [Genode](https://genode.org) — the upstream is
[codeberg.org/genodelabs/genode](https://codeberg.org/genodelabs/genode) —
with **fifteen changes** by **Carrier Detect Labs** on `cdl/main`, plus the
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
| 14 | `5c57cbded2` | tcp_terminal: a half-close from the client is not a close |
| 15 | `e56f190be8` | tcp_terminal: a new connection displaces the old one |

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
controller, so it moves to 11600–11615. The two numbers live in three places
— here and as literals in two run scripts that are not derived from this file
— and a disagreement fails silently and one-sidedly: lwIP picks a
passive-mode data port the router does not forward.

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

### `5c57cbded2`, `e56f190be8` — tcp_terminal: a half-close is not a close

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

The fix keeps the descriptor open for writing, ends only the read side, and
omits an EOF socket from the `select()` read set so the entrypoint does not
spin on a descriptor that is readable for ever.

**The second commit was required by the first, and the obvious form of it
was wrong.**
The listen socket was watched only while no connection existed, so the close
on a zero read was also what returned the session to listening: removing it
left a session that answers exactly one client and is then deaf. Re-arming the
listen socket when the peer stops sending looks like the repair and fails,
because **this stack cannot be relied upon to notice the peer leaving**: a FIN
arriving while the read buffer is still occupied — exactly what a half-closing
client produces — is never reported by a later `select()`. The session now
watches the listen socket unconditionally and a **new connection displaces the
one in place**. That is a deliberate behaviour change against upstream, where a
second client waits in the backlog until the first closes, and it is the half
of this change most worth disagreeing with: it suits an operator interface,
where a stale connection must not be able to lock the port, and it would not
suit a server that expects to arbitrate between concurrent clients.
