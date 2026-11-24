# Runbook for the original EBO SE

Ori reports hardware label `Release`, IPC `v1.7.7-0-g019e668a`, and MCU `v0.0.35-0-gab98488`. Use these to search documentation. Confirm the actual board ID before selecting an installer or image.

## Evidence checklist

| Item | Status | Evidence needed |
|---|---|---|
| Exact board revision | Unknown | Board identifier from vendor documentation or service diagnostics |
| Native IPC application installer | Unknown | A documented route compatible with this board and IPC build |
| C++ toolchain and ABI | Unknown | Vendor compiler and sysroot or equivalent build instructions |
| Camera interface | Unknown | A low-resolution frame source with a monotonic capture timestamp and sequence |
| Factory motion interface | Unknown | Exclusive Follow ownership and explicit manual, docking, sleep, low-battery and obstacle veto inputs |
| Independent motor expiry | Unknown | Factory drive owner or hardware watchdog that cuts output after an expired command, including when the IPC or link fails |
| Removal and recovery | Unknown | Exact model-specific instructions and any package needed to restore stock operation |
| Local owner authentication | Unknown | Existing owner authentication or an installable HTTPS service with a per-owner credential |
| Real footage and robot access | Unavailable in this workspace | Labeled local footage and an operator with physical access to Ori |

Enabot describes upgrades and publishes some system components. The public material consulted did not establish the native IPC, motor or recovery interfaces above. [EBO SE support](https://www.enabot.com/support/ebo-se) · [Enabot source releases](https://www.enabot.com/opensource)

## Integration order

1. Record stock live video, manual drive, Stop, sleep and docking. Confirm the board, installation route and recovery against the exact device. Use the verified route to run a removable, motor-free program that reports build ID and capture frame count. After reboot, verify the program, remove it, and repeat the stock baseline. Record the exact commands and results.
2. Connect the real camera to the C++ core's `Engine::frame` input. Use monotonic **capture** time and latest-frame delivery. Do not stamp delayed frames with a fresh arrival time. Transport the rendered frame ID and actual acknowledged frame through the owner-authenticated phone page. Bind device controls to the home LAN. Use paired-owner authentication or HTTPS with a unique owner credential provisioned at installation.
3. Run Track only for 30 minutes with stock video active. Record available RAM, peak usage, capture-to-decision latency and missed deadlines. Verify still wheels during simulated Follow evaluation and show OFF after reboot.
4. Connect the verified factory motion owner to the core's `Decision` output. The adapter must combine the decision's absolute deadline with the factory limit and pass only remaining time to a drive owner enforcing expiry independently. Recheck active ownership and deadline at delivery. Use `Engine::setInhibit(cause, active)` for each factory condition. Read all initial conditions before enabling controls and revoke motor ownership immediately on takeover. Clear a cause only when the factory reports it has ended. Ending the last condition enables fresh selection. Physical Follow remains disabled until these behaviors can be proven on the actual drive path. The planned integration uses the factory MCU.
5. In a stable cradle, measure drive cutoff and final wheel rotation separately. Test Stop, directions, tracker freeze, motor writer termination, IPC link fault, preview freeze with live heartbeats, phone disconnect, delayed start after Stop and factory manual takeover. A software trace cannot establish physical cutoff.
6. On a clear level floor with stairs physically excluded, select a textured plush target and teach the 1.2 m gap. Across five consecutive retreat-and-stop trials, Ori must turn or advance as needed and settle 1.0–1.5 m from the target without contact. Then exercise approach, occlusion and a crossing distractor. Measure stopping distance. Repeat under supervision with the selected pet only after plush trials pass. [EBO SE manual](https://cdn-files.myshopline.com/file/store/2005826583/1680510886038/EBO-SE-User-Manual.pdf)
7. Record the verified board, package checksum, exact install and removal commands, recovery demonstration, annotated footage results, and stop measurements before tagging a physical Follow milestone.

## Information to request from Enabot

For the original EBO SE, IPC `v1.7.7-0-g019e668a` and MCU `v0.0.35-0-gab98488`, what is the supported way to install and remove a native IPC application? Which interfaces expose timestamped camera frames, factory-controlled drive with expiry, and manual or docking takeover? How can stock operation be restored if the new program fails during boot?

## Return-to-charge verification

Use the original-SE factory docking controller for beacon acquisition, alignment, and charging. The replay core implements the handoff and charging confirmation. A physical adapter needs a verified request, cancel, docking-active, stationary/off-base, and charging or full-on-base indication. Begin in a clear space within two metres in front of the dock. Whole-room navigation is outside the verified operating range. [Enabot EBO SE](https://www.enabot.com/home-robot/ebo-se)

1. On startup, synchronize all factory vetoes and charger telemetry. Serialize engine calls and adapter dispatch. Supply ordered, fresh, monotonic snapshots via `observeDock`, using `Unknown` for disconnected or untrusted telemetry. Call `tick` from an independent regular timer even if camera delivery stops. Interpret `Idle` only as confirmed stationary and off base.
2. At Return home, revoke Follow's drive ownership and drain its pending commands before delivering `takeDockAction()` to the factory. Consume each action once immediately after every input or timer event. Keep Stop and the native factory vetoes available during return. While the factory is driving, the Follow decision HOLD means Follow has relinquished control. Do not continuously send zero-speed commands into the shared motor interface.
3. Observe factory low-battery returns without issuing another start. Initial replay settings allow 3 s for a manual request to be acknowledged by the factory, 60 s for the return, and 1.5 s between telemetry reports. Verify these assumptions against stock timings before deployment. A failed or unacknowledged user request produces a single cancellation. New requests require fresh idle telemetry. A camera view of the dock or contact alone does not establish successful charging. Require 1.5 s of uninterrupted positive charging feedback.
4. Compare stock and integrated behavior from identical marked starts. Run paired trials at 0.5, 1 and 2 m, with straight and angled approaches, three repeats per combination (18 trials per build). Record first-attempt successes, time to stable charging, factory start count, and any repositioning or manual rescue. Include failures in the success denominator. Compare median and 90th-percentile times for successful trials and report the number of successes alongside them.
5. Use stock as the release baseline. Require at least the stock first-attempt success count, at most one start per operator request, zero Follow motor writes during return, and a 90th-percentile time within 10% of stock. Check charging on the robot or charger independently. Separately exercise a blocked approach, interrupted contacts, missed telemetry, Stop, app takeover, and startup while already charging. A loss of telemetry or contact must never report a new success or automatically restart Follow.
