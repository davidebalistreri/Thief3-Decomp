# Buffer provenance for a TDS capture

The experimental combined observer adds `shadow_provenance` to prepared
VB/IB observations. It records only calls intercepted by this observer, even
when they occurred before the selected frame. It never reconstructs missing
bytes or changes shadow-read policy.

`lookup` distinguishes a live tracked wrapper, a live wrapper whose token was
invalidated by a later Reset, and an eagerly retired wrapper recovered via its
existing private marker. An identity or marker miss remains unknown: it is not
evidence that a particular Reset caused the missing token. Marker lookup uses
an already-owned getter reference; the marker does not retain the resource.

`reset_attempts`, `reset_successes`, `reset_failures` and `last_reset` describe
the observed device since the observer started. A resource with known history
also reports `resets_since_creation`; the Lock and write-Unlock counters cover
the latest accepted Reset attempt, or creation when no later attempt occurred.
`last_lock` reports the last intercepted offset, requested size, flags and
native HRESULT in that interval. A zero requested size retains its D3D meaning;
it is not a measured byte count. Calls bypassing the facade are not counted.
No history is claimed if creation or the marker was not observed.

The batch analyzer groups unknown-resource draw observations by these fields
and reports the number of distinct resource pointer values within each group.
Those values are not emitted into the summary and are not guaranteed to be
distinct COM identities. A resource can appear in multiple groups if its
counts change during a frame. Group counts, successful Lock/Unlock calls and
the absence of captured bytes do not prove which bytes the GPU consumed.

Existing traces predate these fields and cannot be retroactively attributed.
