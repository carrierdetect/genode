# The `cdl/main` branch

**If you arrived here from a binary or a source offer, this is the branch that
binary was built from.**

This is a mirror of [Genode](https://genode.org) — the upstream is
[codeberg.org/genodelabs/genode](https://codeberg.org/genodelabs/genode) —
with three commits by **Carrier Detect Labs** on `cdl/main`.

`main` and `staging` are upstream, untouched. The base of this branch is
`492a510242`, **tag `26.05`**, so this is release 26.05 plus the three changes
below and nothing else.

Licence is unchanged: **AGPLv3 with Genode's Section 7 linking exception**, as
upstream.

## Why this fork exists at all

Not to propose anything. It exists so that a binary we publish can be
accompanied by the complete and corresponding source **we host ourselves**,
rather than a patch series plus "clone upstream yourself", which depends on
somebody else's server still offering the same commit.

**These changes are not submitted and should not be: Genode Labs do not accept
LLM-generated patches.** Two of the three are genuine framework defects and the
reports were written up, but they are not filed here and this branch is not a
pull request. If you hit either bug, the place to raise it is
[upstream](https://codeberg.org/genodelabs/genode), described in your own words.

## The three commits

They come from porting the [Hercules](https://github.com/SDL-Hercules-390/hyperion)
IBM mainframe emulator to Genode, where each emulated machine is a separate
component.

### `acc2e4cfe3` — nvme: size the I/O queue allocator to the arrays it indexes

A framework defect, and the one most likely to matter to someone else. The NVMe
driver's I/O queue id allocator was a `Bit_allocator<MAX_IO_ENTRIES>` — 512 bits
— but the ids it returns index `_sq[]`/`_cq[]`/`_dbl[]`, which hold
`MAX_IO_QUEUES + 1` entries. Past `MAX_IO_QUEUES` sessions it hands out ids
beyond the end of those arrays and `setup_io()` indexes out of bounds.
Reachable only past 128 concurrent Block sessions, which is why it has not
bitten. Also stops a rejected submit counting as in flight, and makes the
unreachable arm of the acceptance switch return `REJECTED` rather than falling
through while reporting `ACCEPTED`.

### `7275bf42c4` — vfs/terminal: `read_ready()` must ask the terminal

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

### `7b53634017` — lwip: narrow the ephemeral TCP port range

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
