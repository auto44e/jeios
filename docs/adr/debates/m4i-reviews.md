# M4 PR-I (brokered UART service + D9 demos, ADR 0010 D9) cross-lineage reviews

ADR 0000 part 4 attachment for PR-I, the final M4 PR. AI-assisted review across two
lineages; NOT independent human qualification.

## Design review (before code)
Both lineages confirmed the plan sound and caught four must-fix items, folded in:
the COM2 output must be proven by a QEMU file backend (not self-report; the mode-14
driver used loopback + `null` COM2); UNSUPPORTED needs a kernel path (owner chose
Option A: a DEV_ASSIGN RDX flag + BROKER_REQUEST_HARDENED, ADR D9 note, with an A/B
flex); the timeout needs a supervisor-observable PING/PONG (a client report must
never kill the shared driver); and the revoke proof needs a kernel link between the
#GP and the admin revoke. Also: don't modify the PR-H-tested uart_driver.c (add
uart_service.c / uart_client.c); a separate JE_SVC_UART_RESTART_LIMIT. (The
D7-SEND-only-vs-SERVICE_BIND "conflict" was already resolved in-code: services get a
bidirectional SEND|RECV control channel.)

## Code review (Codex-authored impl + lead continuation) — no Critical/High bugs;
## evidence + cleanup gaps
Both lineages confirmed the mechanism sound: device handle trusted only from ATTACH
on CONTROL; capability-aware control recv; bounded port I/O; real TX (MCR 0x0b);
broker replies demuxed by channel; the hardened syscall decode accepts only the one
flag and the refusal returns before any binding side effect; detach vs revoke are
genuinely distinct; waits wrap-safe; uart_driver.c byte-identical. The lead had
already mutant-verified the two headline proofs (hardened-refusal-removed and
revoke-deny-removed both FAIL mode 15, the latter leaking R! to COM2). Fix-pass
items (all evidence/cleanup):

- M1 (Claude): the PING/PONG timeout is never exercised on a HEALTHY incarnation
  (PING only goes to an already-hung one, and HANG closes the brokered channels
  itself, so the client's PEER_CLOSED comes from the service, not the timeout
  kill). A broken PONG handler would still pass. Fix: PING healthy incarnations and
  require PONG before HANG and after each restart; HANG must not close the client
  channels; give the client's post-I1 wait >= 2x the deadline.
- M2 (both): the hardened "no side-effect" snapshot writes before/after adjacently
  (compares a value with itself) and omits the IRQ3 mask + TSS bitmap. Snapshot
  before-at-top and after-at-return, including je_arch_tss_com2_io() and
  je_arch_irq3_is_unmasked().
- M3 (both): the hardened-zero-caps / restarted-delivery checks depend on
  supervisor-authored op labels (GRANT/RESTARTED) and shape; a cap on an UNAVAIL is
  invisible, and the shape-independent counter is unused + double-counted.
  Corroborate per CLIENT IDENTITY from the shape-independent cap-delivery counter:
  hardened client 0 caps, normal client exactly its grants.
- Codex-High: send_after_revoke treats an immediate PEER_CLOSED as revoke proof —
  require the send to return OK first, THEN PEER_CLOSED. And SERVICE_BIND must
  validate bind.generation == the service's current generation (a stale bind from
  an old incarnation must be rejected).
- M4/Codex: exact revoke-death accounting (separate the admin-revoked-owner-death
  from a real death-revoke; require exact counts) and tie the vec-13 fault to the
  revoked UART incarnation (owner id + pending consumed; ideally the faulting RIP is
  an in/out).
- M5/Codex: the scenario deadline is only checked between iterations and is too
  small for the summed blocking waits; make it a real aggregate with per-phase
  sub-deadlines that FAIL loudly (not via the kernel monitor watchdog).
- M6 (Claude): the HANG apply races the kernel monitor; wait for a kernel ack
  before PING.
- Cleanup (both): handle leaks in serve_request (reply-send-fail), the DEGRADED
  attach path, and send_after_revoke; check client exit codes (0x5548 hardened,
  JE_SVC_UART_DONE_EXIT normal); validate reply.reserved; clamp close_minted; reply
  EXHAUSTED rather than failing the scenario when a client's grants are spent; add
  an I3 write on the gen3 channel so recovered TX is proven end-to-end (I1I2I3).

Disagreement resolved: Claude (M1) is right that HANG-closes-channels masks the
PING/PONG proof; Codex treated it as sound. The fix keeps HANG from closing the
client channels and drives the kill from the supervisor's deadline.

Verification after the fix: see WORKER_NOTES + the PR. Local only; remote CI on the PR.
