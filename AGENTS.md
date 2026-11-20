# AGENTS.md

## Development Workflow

- Default to the observed failure -> minimal relevant fix -> one scoped verification set -> delivery. Diagnose the earliest failed prerequisite before changing later stages. A mock that assumes a working prerequisite does not validate that prerequisite on real Steam or hardware.
- Follow DEVELOPMENT.md §10 for verification scope and stop conditions. Routine work does not require full suites, both target rebuilds, sanitizers, a clean checkout, repeated device launches, or a separate design/approval round. Add checks only for a concrete affected behavior or unresolved defect; reuse passing results until relevant changes invalidate them.
- Deliver when the requested behavior and the applicable focused checks pass. Do not delay delivery for repeated audits, expanded hypothetical cases, lengthy documentation, or serial Issue updates. Record material evidence once, concisely; state unavailable runtime evidence without claiming success or blocking independently usable work.
- Read the applicable rules and relevant source/reference sections; do not reread all kickoff/decision documents every session. Keep the active feature or repair in scope while improving workflow.
- Documentation-only edits need a diff/content check, not code builds or runtime tests. PC-tool-only edits do not require rebuilding or redeploying the Switch client. Keep runtime diagnostic parameters adjustable rather than rebuilding for each value.
- Build the Switch client with `make switch` / `make switch-diag`; deploy with `make switch-deploy SWITCH_IP=<confirmed address>`. The deploy entry builds `nsteamlink_nro`, snapshots it with its hash, and sends that same file. The CMake `nsteamlink` target only updates ELF; never use it as evidence that the runnable NRO changed or manually pair it with a pre-existing NRO.

## Evidence Discipline

- Treat user-provided real-device observations, screenshots, logs, and exact console text as primary evidence.
- Do not override a user-observed Switch screen state with PC-side tool behavior. For example, `nxlink` exiting normally is not evidence that the Switch returned to hbmenu.
- Before changing protocol flow, read the relevant project docs and source references first. Record which file or log supports the conclusion.
- Separate statements into:
  - Evidence: directly observed logs, screenshots, source lines, command output, or documented behavior.
  - Conclusion: what the evidence proves.
  - Hypothesis: plausible but unproven explanations that still need validation.
- Do not present hypotheses as conclusions. Mark them as `hypothesis` or `待验证`.
- If a prior conclusion is contradicted by new evidence, explicitly retract it in docs before building on it.
- For Switch testing, do not use TCP port probes against netloader port `28280`; use `nxlink` directly because empty TCP connections can consume netloader's receive attempt.
- This project's Switch debug unit is `10.10.10.17`; hbmenu netloader listens on port `28280`. Do not infer another Switch IP from ARP/neighbour tables unless the user explicitly says the IP changed.
- Keep Steam authentication semantics aligned with `docs/STEAM_REMOTE_PLAY_AUTH.md`: do not collapse pairing PIN / authorization code and connect/security PIN into one concept, and do not change protocol flow without a source/log-backed evidence note.
- Current tasks and status live exclusively in GitHub Issues on the repo `kxn/nsteamlink` (with milestones). Repo documents carry only macro design/spec (kickoff, DEVELOPMENT, decisions), user manuals (README, SWITCH_SETUP, UDP_DEBUG, third_party/README), and protocol/platform references (STEAM_REMOTE_PLAY_AUTH, M3_RESEARCH_PLAN, GFX_MESA_INVESTIGATION). Never maintain progress/status/"下一步" sections in documents. The old M2/M3/M4_STATUS progress docs were removed on 2026-08-28; retrieve them from git history if needed.

## Switch Homebrew Runtime Discipline

- Device testing must be self-contained: the user only prepares hbmenu netloader. Automatically deploy fixtures, collect machine-readable stages/results/errors and any needed readbacks, and diagnose failures without asking the user to observe flashes, transcribe logs, or report screen state. Unobservable loader/display properties remain explicitly unverified; never infer them from nxlink exit.
- Never ask the user to manually copy files to the Switch SD card. Bundle test fixtures/resources into the NRO (for example via RomFS), or transfer them automatically through available tools. Prepare this delivery path before requesting a device test; the user's one-time manual copy is not permission to repeat that workflow.
- Treat returning to hbmenu as returning to a loader ABI, not as proof that the process state was fully reset.
- Before any Switch NRO returns from `main()`, it must satisfy the Homebrew ABI cleanup rule: no leaked handles, no dirty memory-state assumptions, and no leftover background threads.
- Do not use `pthread_detach()` or fire-and-forget threads in Switch NRO code unless a decision record proves they terminate before returning to hbmenu. Prefer joinable threads and explicit `stop -> join -> destroy`.
- Cleanup order for Switch probe/client code must be evidence-backed and logged. Stop/join app-owned threads before destroying their dependencies; stop/join IHS session/client workers before `IHS_Quit()` / `socketExit()`; release SDL/Mesa resources before returning to hbmenu.
- Runtime diagnostics must never sit on the rendering, input, or local-exit hot path. nxlink/stdout logging must be non-blocking or bounded; high-rate IHS/session logs must be rate-limited before testing interactive streaming.
- Request a second launch only for a concrete lifecycle risk, a new related failure, or explicit lifecycle acceptance. Merely touching a file that uses threads, sockets or SDL/Mesa does not require it. Explain the evidence value before spending the user's time; unavailable device testing limits confidence, not all development or delivery.
- `nxlink` exiting normally is never sufficient evidence that the Switch screen returned to hbmenu. If the user declines hbmenu/second-launch testing, record PC-side cleanup as partial evidence and leave lifecycle safety as lower-confidence instead of forcing another manual test.

## Known Project Pitfalls

- `gamesRunning=0` in Steam discovery logs means Steam was discovered but no game was running. It is not evidence that UDP broadcast failed.
- Steam Remote Play has separate pairing authorization code and connect/security PIN semantics. Re-read `docs/STEAM_REMOTE_PLAY_AUTH.md` before changing auth UI or protocol flow.
- The Switch test unit is normally in full application mode when the user says it is. Do not re-litigate applet mode unless a new observation directly points there.
- SDL2/Mesa work should follow the upstream/devkitPro example lifecycle unless a decision record documents why this project must diverge.
- HID/input diagnostics can create control-channel retransmission log floods. If frames or local `+` freeze only after pressing controls, check log backpressure and main-loop drain behavior before changing protocol semantics.

## Development Verification Cost

- Use incremental builds and choose one verification set appropriate to the changed modules. Do not rerun passing checks without new edits, failures, or unresolved concerns. Reserve full unrelated suites for changes that actually cross those boundaries or a release.
- A dirty checkout does not block a diagnostic device experiment. Record commit, dirty state, artifact hash and the tested patch; keep clean/tag requirements for release artifacts.
- When only a PC diagnostic tool changes, do not rebuild or redeploy the Switch client. Make diagnostic values adjustable at runtime rather than rebuilding for each value.
- Distinguish actual interface/protocol constraints from implementation policy. Do not reject a device parameter based solely on an undocumented convenience assumption.
