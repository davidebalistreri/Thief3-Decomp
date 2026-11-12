# D3D8 declaration observation prototype

These portable helpers are used by the proxy's optional declaration observer;
the integration has not been validated against TDS. `ScanDeclaration` frames an application-supplied D3D8 declaration into DWORDs,
including its terminator. It advances past the payloads of CONST and EXT tokens
before looking for END; payload words can themselves equal `0xffffffff`. It
does not validate the semantics of the declaration or decode shader programs.

The caller must supply a fault-safe reader that returns true only for an exact
copy, for example a checked `ReadProcessMemory` call on Windows. The scanner
never dereferences the remote pointer, rejects address overflow and partial
reads, and has an explicit 1–4096 word limit. Failed scans expose no partial
words. The data is sampled from application memory and may differ from what a
driver consumes if another thread mutates it.

`VertexDeclarationRegistry` assigns independent, non-reused device lifetime
tokens and tracks observed numeric handles with generations and bounded memory.
Begin/end tickets are intended to bracket intercepted Create/Delete/Set/Reset
and draw calls without holding registry locks across any driver call. Overlap
poisons that device's observations; Delete and Reset mark known handles
unavailable before the native call, even if it later fails. In the optional
history mode only, complete available payloads are quarantined across a
successful Reset under the same per-device budget. A successful explicit Set
of the same numeric value after that Reset can expose a separate pre-reset
hypothesis; it never restores the standard candidate. Failed or unknown Reset
results, Delete attempts, collisions and unsafe overlaps discard that payload. A failed Delete
retains an unavailable tombstone; only a successful Delete removes it. Reset
retains tombstones because shader survival across Reset has not been verified.
If a successful Create returns an already-observed handle, its declaration is
unavailable rather than silently replacing the earlier one. A successful Create
with an unreadable output handle poisons the device. Failed declaration scans
are stored as unavailable rather than exposing guessed bytes. An unavailable
generation keeps the pre-create scan status and the most recent invalidation
(`scan_incomplete`, `invalid_snapshot`, `handle_collision`, `reset_attempt` or
`delete_failed`). Reset is recorded before forwarding, even if it fails; the
scan status survives that invalidation. `observation_error` instead describes a
single failed observation copy: it is not stored on the generation, and the
next draw may still produce a candidate. These fields are diagnostic
observations, not proof that the driver consumed the snapshot.
Allocation/capacity exhaustion can instead poison the device, leaving no
stable generation-level reason; no cause is fabricated for that case.

Device and handle metadata are preallocated before the registry is published
(at most 64 devices, 4096 handles per device and 65536 slots total). If those
limits or metadata allocation fail, no device can be opened. Handle lookup is
bounded and linear. Payload allocation and copying, and release of retired
payloads, occur outside the mutex. A reentrant allocator can still invalidate a
device or make an observation fail; it cannot make the registry trust stale
data. Retirement uses a preallocated queue and a bounded, non-recursive drain.

The per-device DWORD budget stays charged while payload allocation is in
progress, while an observation pins a retired payload and until its final
deallocation returns. A closing device slot cannot be reused during those
periods. The input snapshot, returned observation vector, allocator metadata
and container metadata are caller/process memory outside that DWORD budget;
it is not a cap on total process memory. Publish the registry only after its
constructor finishes and destroy it only after all calls have ended. Tests
exercise reentrant allocation/deallocation and concurrent close, but this
integration has only been tested with host mocks, not against TDS.

An observation from `Observe` is usable only if the corresponding draw's
`End` succeeds. Even then, matching a numeric value to a recorded creation is
only a candidate: an FVF can resemble a handle, a native alias may bypass
interception, state blocks may change bindings, and the snapshot may race the
driver. The proxy invalidates observations on detected alias escapes and state-block
calls, but cannot detect every native alias. These helpers do not establish TDS
replayability.

```sh
python3 -B -m unittest t3render/tests/test_vertex_declaration.py -v
```
