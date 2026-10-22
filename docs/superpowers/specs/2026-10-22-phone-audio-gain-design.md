# Phone-only audio gain design

Date: 2026-10-22

## Intent and scope

Give the existing C++ AudioPlaybackConnector a real phone-only volume control. The slider must make 0% silent, preserve the current normal phone loudness at 50%, and provide approximately 2× digital signal gain at 100% for quiet material. The phone's own volume remains an independent upstream control. The slider must not alter the PC master volume or other applications' sessions. Multiple phones may play in one application process.

Keep the current Bluetooth `AudioPlaybackConnection` lifecycle and Windows-managed A2DP codec negotiation. Do not install a virtual driver, rewrite the app in C#, change the Bluetooth radio, or update the Realtek driver. The separate `OpenAsync` failure `0x8007001F` is not claimed to be fixed by this work.

## Evidence and constraints

The current slider uses `ISimpleAudioVolume` on the AudioSrv-owned phone render session. Windows limits that control to 0–1. On the test PC, the neutral reference is already 1.0, so slider values from 50% through 100% all set the session to the same ceiling.

A throwaway live probe on the user's iQOO Neo10 Pro+ captured its A2DP input as 44.1 kHz stereo float PCM even while the existing phone render session was muted. It converted and rendered a five-second 2× preview to the default 192 kHz stereo output with no reported dropped frames; the user heard it as clearly louder and normal. This proves feasibility on the current connection, not production stability or universal endpoint support. The probe lives only in ignored `build-artifacts/volume-probe` and is not product code.

## Chosen approach

Add an in-process, per-phone WASAPI processor. Each processor captures only its phone's A2DP input endpoint, applies a gain curve and limiter, and renders in shared mode to the default multimedia output selected when the phone connects. Windows can provide render-side sample-rate conversion with `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` and `AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`; if initialization rejects a source format, the processor may negotiate the output mix format and use a tested in-process converter. No external dependency or driver is required.

The existing PnP-parent lookup identifies the phone's capture endpoint. Because an endpoint may be absent from an enumeration snapshot yet openable directly, the processor must resolve its exact endpoint ID and open it directly; it must never guess another microphone. Each phone has its own capture and render state, while all workers remain in the single application process and share only the global slider value. Windows mixes their render streams with other PC audio without letting this slider change those other streams.

Alternative approaches were rejected: retaining session-only volume cannot boost above 1.0, and an APO or virtual-device solution would require a new system-level installation and maintenance path.

## Audio routing and lifecycle

For slider fraction `s`, target gain is `2s` for `0 ≤ s ≤ 0.5` and `1 + 2(s - 0.5)` for `0.5 < s ≤ 1`. Thus 50% is unity and 100% is 2× before limiting. Apply a bounded, smooth peak limiter to avoid hard clipping; loud material near full scale may gain less than 2×. Keep the gain computation and limiter independently testable from device I/O.

After `AudioPlaybackConnection` reaches `Opened`, prepare the processor's capture and render clients on a background audio worker. Do not emit processed sound while the Windows-owned phone render session is still audible. Once the matching phone session is found, save its relevant mute/volume state, silence that session, then enable processed output. The probe confirmed that this silence does not silence the A2DP capture samples. If the phone session has not appeared yet because the phone is paused, wait for it without guessing a different session or mixing two audible copies. A start-up failure disconnects that phone and reports the specific stage instead of advertising a working boost.

On intentional disconnect, application exit, or processor failure, stop the processor, restore any phone session state changed by the app when the session still exists, then close that phone's `AudioPlaybackConnection`. Restoration must not touch unrelated sessions. Preserve a visible error reason after a failure-triggered disconnect. Avoid doing device initialization, stream waits, or teardown while holding UI or global connection locks.

Bind each processor to the default multimedia render endpoint present when its phone connects. If that default output changes, immediately stop the processors bound to the old output, restore their phone-session states, and disconnect those phones; do not reroute or retry on the new output. Because all currently connected phones normally share that default output, the switch normally disconnects all of them. A later manual connection binds to the new default output. For sleep/wake with the same default output, pause the affected renderer and allow one rebuild retry within five seconds. If rebuilding cannot restore a safe single-path output in that window, disconnect only the affected phone. Other PC applications remain running. Never reset the Bluetooth adapter automatically.

The render stream must use bounded buffering, target no more than 100 ms of added processing/buffering latency under stable conditions, and account for capture/render clock drift. Its worker may wait for audio events but must not block the UI thread. A phone that abruptly turns off Bluetooth must cause its worker to stop promptly, without leaving a muted AudioSrv session or an unresponsive window. Audio samples stay in memory; the feature neither records nor writes them to disk.

## UI and settings

Keep the existing 0–100% slider and default 50% setting. Display an unambiguous connection/processing error if enhanced routing fails; do not show a connected state or a 100% boost that is not actually operating. Advance `volumeControlVersion` and migrate the existing stored slider value without importing the old per-session neutral-reference model as a gain reference. Settings and logs remain in the app's existing per-user data directory.

## Verification and acceptance

- Automated tests cover the 0/50/100% gain mapping, midpoints, limiter behavior on silence and near-full-scale signals, buffer bounds, per-phone isolation, and teardown state restoration.
- Build the existing x64, x86, and ARM64 configurations in CI. No product dependency or elevated installer is added.
- On the user's x64 PC, confirm 0% silence, 50% current normal loudness, and audible improvement at 100% with phone volume unchanged. Verify other PC audio and master volume remain unchanged.
- Soak test continuous playback, pause/resume, abrupt phone Bluetooth shutdown, and sleep/wake. Switch the default output and verify all phones bound to the old output disconnect promptly, can be manually reconnected to the new output, and leave no muted session or duplicate sound. Verify no unresponsive UI or unbounded reconnect loop. Test two simultaneous phones when hardware is available; absence of a second phone is reported as an unverified case, not a passed test.
- Reproduce `0x8007001F` only if it occurs naturally during testing. Record `OpenAsync` status and extended error; do not claim this gain path resolves it. A separate recovery design will need a live fault to validate any device-level reset.

## Follow-up boundary

The current `0x8007001F` investigation remains a separate compatibility task. The user declined a Bluetooth driver update. An app-level future change may improve diagnostics and offer a user-approved, narrowly targeted device recovery action, but must be tested during an actual failure before it is integrated or described as a fix.
