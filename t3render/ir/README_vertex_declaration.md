# D3D8 declaration observation prototype

These portable helpers are not connected to the proxy or the game.
`ScanDeclaration` frames an application-supplied D3D8 declaration into DWORDs,
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
unavailable before the native call, even if it later fails. A failed Delete
retains an unavailable tombstone; only a successful Delete removes it. Reset
retains tombstones because shader survival across Reset has not been verified.
If a successful Create returns an already-observed handle, its declaration is
unavailable rather than silently replacing the earlier one. A successful Create
with an unreadable output handle poisons the device. Failed declaration scans
are stored as unavailable rather than exposing guessed bytes.

The registry currently allocates, copies and releases standard container
storage while holding its mutex. A global allocator that reenters this registry
could deadlock. Resolve and test allocator reentrancy before integrating these
helpers into the game process.

An observation from `Observe` is usable only if the corresponding draw's
`End` succeeds. Even then, matching a numeric value to a recorded creation is
only a candidate: an FVF can resemble a handle, a native alias may bypass
interception, state blocks may change bindings, and the snapshot may race the
driver. The future integration must invalidate observations for untracked
aliases and state changes. These helpers do not establish TDS replayability.

```sh
python3 -B -m unittest t3render/tests/test_vertex_declaration.py -v
```
