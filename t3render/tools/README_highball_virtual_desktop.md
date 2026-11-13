# Highball virtual desktop experiment

On 2026-11-13 the user verified switching from an 800×600 Wine virtual desktop
containing Notepad to a macOS application and back. This did not launch TDS,
exercise Direct3D, or establish whether the game keeps rendering in the
background. See the [result](../benchmarks/runtime/2026-11-13-virtual-desktop/result.json).

The test did not change a persistent Wine desktop setting. With an existing
Highball engine and bottle, the relevant command was:

```sh
env WINEPREFIX="$T3RENDER_BOTTLE" \
  WINESERVER="$T3RENDER_ENGINE/engine/bin/wineserver" \
  DYLD_FALLBACK_LIBRARY_PATH="$T3RENDER_ENGINE/frameworks:$T3RENDER_ENGINE/frameworks/GStreamer.framework/Versions/1.0/lib" \
  DYLD_FALLBACK_FRAMEWORK_PATH="$T3RENDER_ENGINE/frameworks" WINEDEBUG=-all \
  "$T3RENDER_ENGINE/engine/bin/wine" 'C:\windows\explorer.exe' \
  /desktop=T3RenderTest,800x600 'C:\windows\system32\notepad.exe'
```

For a later TDS test, Steam and its children would need to start in the same
virtual desktop; an already-running Steam may launch the game outside it.
On a [follow-up TDS run](../benchmarks/runtime/2026-11-13-tds-virtual-desktop/result.json),
the user could switch to Codex more easily, but reported a window that still
appeared on top and lacked title-bar controls. The game's D3D presentation was
still fullscreen inside the virtual desktop. A marker did not produce a frame
while the user was in Codex; returning focus produced the trace. The cause of
the background delay was not isolated. Close or restart Steam only when
convenient for the user.

T3SDK also offers `Display.Borderless=1` and `PauseInBackground=0`, but its
loader is not currently installed in this bottle and compatibility with the
T3Render proxy on Highball is unverified. Do not assume either option makes the
game run in the background until a real capture checks it.
