ADJUST. The lifecycle-only/no-run-queue decision is sound, but the proposed seam is not yet a real ownership boundary.

The single biggest risk is dual ownership of lifecycle state. Today `threads[]` contains `kind`, VM/process identity, wait state, death record, run state, and frame together ([sched.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:20)). If `proc_lifecycle.c` merely accesses those fields through getters/setters, it owns policy but not state. If it mirrors them, there are two authorities. `set_dying()` would especially blur the line by mutating both run-queue state and lifecycle state.

The extraction needs a lifecycle-owned per-slot record containing at least:

- process identity, VM/index link, lifecycle state, wait ownership, and terminal record;
- an opaque scheduler-context token;
- an opaque VM/kernel-stack ownership token.

`sched.c` and the ARM adapter should retain only runnable state, frame/context storage, stack mechanics, and dispatch state. The lifecycle module must be the only production code calling `je_proc_link`, `je_proc_on_dying`, `je_proc_on_reaped`, registry mutation, and waiter release. Currently rollback still calls those primitives directly in [spawn.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/spawn.c:95), and bootstrap paths do so in `main.c`; those would otherwise remain second lifecycle owners.

### `je_thread_ctx_ops`

The proposed per-field API is too granular. Separate calls such as `thread_kind()`, `thread_proc()`, `thread_vm()`, and `is_current()` can observe an inconsistent composite state and make the seam a hidden dependency graph.

Prefer:

- one bounded `snapshot_slot(slot, view)` returning kind, run state, identity, VM/index, frame-present, wait-present, and an opaque context token;
- one atomic `reap_prepare(slot, ticket)` that checks current context, active address space, active kernel stack, interrupt state, and returns a ticket;
- `capture_death(slot, event, record)` before the frame/context is cleared;
- `make_unschedulable(slot)` that changes only run-queue state;
- `clear_wait_state(slot)`;
- `restore_stack_guard(ticket)`;
- `clear_scheduler_slot(ticket)`.

Return explicit statuses distinguishing caller misuse, user-visible invalidity, busy/liveness refusal, and kernel invariant failure. A boolean `is_live_address_space()` is not enough.

### `mark_dying`

The lifecycle module should own the ordering policy; the adapter should own execution-context mutation.

The required sequence is:

1. Capture immutable identity and death data without mutation.
2. Release the endpoint waiter/reservation.
3. Run the device death notification.
4. Transition the PROCESS record with `je_proc_on_dying`.
5. Clear wait state and make the scheduler context non-runnable.
6. Publish the terminal lifecycle record.

The current implementation releases first in [sched.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:256), but `je_sched_drop_user()` also calls the waiter-release hook ([sched.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:572)). That contradicts a literal “sole release owner.” Reaping should assert that the waiter is already absent; it should not release again.

Also preserve the distinction between the death notification in `mark_dying()` and the later VM teardown hook: `kep_teardown()` independently invokes device death handling during `je_vm_destroy_owned()` ([endpoint_kern.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/endpoint_kern.c:47)). Do not accidentally merge or reorder those effects.

### Kill and reap

Kill policy can live in the neutral module, provided it consumes a complete slot snapshot. The READY/BLOCKED_IPC/frame-present gates are not inherently x86, but the lifecycle module must not interpret a raw `je_trap_frame`.

Reaping should stay neutral at the transaction level:

`reap_prepare → restore guard → detach/clear scheduler slot → destroy VM-owned process → on_reaped`.

The current ordering and failure behavior are important ([sched.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:607)). In particular, guard-restore failure leaves the slot intact, while VM-destroy or `on_reaped` failure after detachment is fatal.

Do not calculate a guard address from `rsp0_top` in neutral code. The x86 implementation currently relies on `TSS.RSP0` and `STACK_PAGES` ([sched.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:614)); ARM cannot assume that `SP_EL1` equals a stack top. Use an opaque architecture-produced ticket.

### Keep names or rename?

Keep the existing `je_sched_*` names for this PR. The header is public and the callers are numerous ([sched.h](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/include/jeios/sched.h:12)); renaming adds churn without reducing lifecycle risk.

Implement new internal `je_lifecycle_*` functions and retain `je_sched_*` as compatibility wrappers. The wrappers must contain no lifecycle state and must not become a second decision layer. Rename later only as part of a separate API cleanup or run-queue migration decision.

### ARM reuse requirements

The seam is insufficient unless it includes:

- current slot and scheduler-context identity, without assuming slot zero is always the bootstrap role;
- a normalized run-state view including BLOCKED_IPC;
- opaque address-space and kernel-stack identity;
- atomic reap-safety validation for TTBR0 and the active SP_EL1 stack interval;
- architecture-neutral death-record storage or an opaque bounded payload;
- ARM adapter capture of ELR/SPSR/ESR/FAR/vector;
- no `JE_INT80_LEN`, `cs/ss`, RFLAGS, or `je_trap_frame` types in neutral code;
- architecture-specific wait cancellation/continuation clearing;
- VM-owned stack-guard restoration and `destroy_owned` callbacks;
- user-context creation/link/rollback hooks, since the current link is embedded in `je_thread_create_user_start()` ([sched.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/core/sched.c:224));
- separate idle handling: `sti; hlt`, `wfi`, and SVC entry/return remain outside lifecycle code.

The ARM path currently has an opaque frame model already: `je_rr_ctx` stores `void *frame`, bounds, and phase ([sched_rr.h](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/include/jeios/sched_rr.h:35)), while the ARM validator owns the 304-byte frame details ([sched_arm.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/sched_arm.c:220)). The lifecycle interface should follow that pattern.

### Tests and mutation

The no-regression gate is necessary but not sufficient. The current kill-all/reap test uses a waiter-release fake that always succeeds ([test_sched_killall.c](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/tests/test_sched_killall.c:172)); it will not prove that release happened before state destruction.

The minimum additional assurance is:

- an event-order fake asserting waiter release precedes every lifecycle/context mutation;
- a mutant moving waiter release last, which must fail;
- a duplicate-release mutant, which must fail;
- mutants accepting RUNNING/SLEEPING/ZOMBIE victims;
- mutants skipping active-address-space or active-stack guards;
- rollback mutants covering link failure and guard restoration;
- registry mutants covering same-VM/different-ID registration and stale unregister;
- tests proving VM destruction occurs only after scheduler detachment and before `on_reaped`.

A neutral fake suite alone is not “exercised by both arch paths.” Add an x86 adapter test and an ARM/RR adapter test. If the real ARM user adapter does not yet exist, label that evidence as adapter-contract coverage, not ARM userspace lifecycle parity.

**BOUNDARY SOUND / ADJUST:** keep the lifecycle-only extraction and `je_sched_*` wrappers, but replace the field-level callback façade with a lifecycle-owned per-slot table plus atomic slot snapshots, reap tickets, opaque death records, and explicit terminalization/rollback hooks. Remove all second waiter-release and direct production PROCESS-transition paths before coding.
