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
