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
the background delay was not isolated. After TDS exited, the desktop window
had no caption or resize frame; a separate Wine process attempting to change
its style received `ERROR_ACCESS_DENIED` and left it unchanged. A decorated
game window remains unverified. Close or restart Steam only when convenient for
the user.

The user later confirmed that these launches interrupt other work: a Wine
window stays above other apps and the game may start minimized. The launcher's log
for the guarded 2026-11-17 run confirms `Full screen mode`, a 1024×768 display
mode change and an attempt to restore the 1728×1080 mode. This is not evidence
of why the game starts minimized. Do not assume the virtual desktop prevents
focus or display-mode changes; avoid starting these interactive tests while the
user is working. A non-intrusive, decorated game window remains to be tested.
Its acceptance check must include another app covering the game normally, the
game remaining visible when brought back, and live rendering continuing for
30–60 seconds after focus changes. The SDK's `Borderless` setting currently
sizes the game window to the whole monitor and explicitly brings it forward;
it is not by itself an implementation of that behavior.

T3SDK also offers `Display.Borderless=1` and `PauseInBackground=0`, but its
loader is not currently installed in this bottle and compatibility with the
T3Render proxy on Highball is unverified. Do not assume either option makes the
game run in the background until a real capture checks it.

A [hidden-window Wine probe](../../build/t3render-diagnostics/M1-sdk-proxy-hook-20261117.md)
successfully installed SDK-style MinHook detours on the proxy's `CreateDevice`
and `Reset` methods, created a windowed D3D8 device, reset it, and captured one
frame. It did not load the actual SDK or scene bridge, activate a window, or
launch TDS; it does not establish background rendering or live compatibility.
