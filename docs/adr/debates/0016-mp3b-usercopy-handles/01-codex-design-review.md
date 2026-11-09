Reviewed at `main` HEAD `0cfcde0c29e0144decafde97b0af8de014bebdd4`; no files changed and no tests run.

| Decision | Verdict | Finding |
|---|---|---|
| D1 | FIX | The leaf `ret` model is correct, but valid faults currently hit the double-fault guard before fixup. |
| D2 | FIX | Arena bounds are the right portable replacement; DFSC matching is too broad and misses required ISS gates. |
| D3 | OK | A two-argument ARM shim around the existing three-argument API is clean. |
| D4 | FIX | ABI extraction is sound, but must preserve x86 typed close/dup lifetimes and avoid dragging x86 glue into ARM. |
| D5 | ESCALATE | This changes the tested slot-4 path, restore seam, and guard semantics. |
| D6 | FIX | Good evidence shape, but the negative injections and independent mutant profiles must be made explicit. |

## Critical blocker: the guard currently prevents recovery

A real copy fault occurs while handling EL0 SVC:

- `vectors.S:370-375` sets `je_arm_in_handler = 1`.
- `je_arm_el0_svc()` performs the syscall while that guard remains set.
- A nested copy DABT enters `je_arm_vec_common`.
- `vectors.S:90-95` sees the guard and immediately branches to `je_arm_double_fault`.

Therefore D1/D5 cannot work as written. The valid copy case needs a narrowly authorized nested-fault path. It must preserve the outer guard while returning to the copy site; the existing restore seam clears the guard at `vectors.S:246-249`, which would be wrong for a nested copy recovery.

The fix must either:

- add a dedicated copy-fixup restore path that preserves the outer guard, or
- introduce an equivalent explicit guard-depth/state mechanism.

Clearing the guard around usercopy would weaken the tested nested-fault invariant and is not the minimal safe choice.

## D1 — ARM assembly

The leaf logic is sound. `LDTRB`/`STTRB` with `x0=dst`, `x1=src`, `x2=len`, a caller-saved temporary, and untouched `x30` will resume correctly:

1. the common saver preserves `x30` at `vectors.S:116`;
2. fixup rewrites `ELR_EL1`;
3. the restore seam restores `x30` at `vectors.S:290`;
4. `ERET` resumes at `je_copy_user_fixup`;
5. `mov w0,#1; ret` returns to the original C caller.

This only holds if the recovery path restores the entire trap frame and does not directly return from C.

`LDTRB`/`STTRB` are preferable to plain `LDRB`/`STRB`: plain EL1 accesses can have stronger EL1 permissions than EL0. The unprivileged forms enforce the user access class. The resulting fault remains an EL1 current-EL data abort (`EC=0x25`); `WnR=0` for the load and `WnR=1` for the store.

## D2 — scope and predicate

Replacing x86 `canonical()` with the authoritative arena-range check is correct and minimal. The arena is already architecture-selected through `JE_VM_ARENA_BASE/SIZE`; accepted pointers are below the target VA limit.

The proposed DFSC rule needs tightening. `0b0001xx` means DFSC `0x04–0x07`, including translation faults at levels 0, 1, 2, and 3. Current ARM user mappings are 4 KiB leaves, and `arm_read_user_pte()` directly validates the L3 descriptor (`vm_backend.c:471-513`). A level-0/1/2 fault can indicate a broken root or missing table, not an absent user leaf. Recovering those would mask kernel page-table corruption.

Recommend:

- require `DFSC == 0x07` (translation fault, level 3);
- require `IL == 1`;
- reject `S1PTW`, `CM`, `EA`, and `FnV`;
- retain exact ELR, FAR-span, direction, active-scope, and owner checks.

`span_pages_ok()` already requires `JE_VM_PERM_RW_NX` for stores (`usercopy.c:78-93`). A normal RO store therefore fails prevalidation. If a permission fault nevertheless occurs, translation-only matching leaves it fatal, which is correct.

The arch hook must receive a core-owned scope snapshot or explicit scope fields. It must not duplicate or reach into the private `g_scope` independently.

## D3 — current-user shim

This is clean. Keep `je_arm_sched_current_user(vm,index,slot)` for ARM-internal/test consumers and add:

```c
je_status je_sched_current_user(je_vm **vm, uint32_t *index)
{
    uint32_t slot;
    return je_arm_sched_current_user(vm, index, &slot);
}
```

The implementation belongs in the ARM scheduler layer, not the x86 core scheduler. It should still reject the EL1 idle context and return success only for the current user process.

## D4 — frame-neutral syscall glue

The result ABI is sufficient:

- SELF/DUP: 32-bit handle in result;
- QUERY: object type;
- RIGHTS: rights mask;
- CLOSE/COPY_PROBE: result zero;
- endpoint-create, though deferred, fits as two packed 32-bit handles.

The extraction must preserve x86 behavior. In particular:

- `HANDLE_CLOSE` is typed at `syscall.c:818-860`; raw `je_handle_close()` on an endpoint would leak its reference.
- `HANDLE_DUP` has SELF, endpoint, and process-specific lifetime behavior at `syscall.c:901-972`.
- The x86 pending-recv continuation at `syscall.c:753-769` must remain x86-only.
- ARM must stop converting every non-RETURN/YIELD/EXIT action to `UNSUPPORTED` (`sched_arm.c:1099-1103`) and instead dispatch only the approved MP3b actions.

The current ARM build compiles `syscall.c` with `JE_SYSCALL_HOSTED_TEST` (`mk/aarch64.mk:226-228`) and links only the decoder. It also does not link `usercopy.c` or ARM usercopy assembly. A separate frame-neutral MP3b translation unit, or an explicitly bounded build split, is required.

## D5 — owner escalation

Recommend Option A, but with the guard correction above.

Escalation should state:

- `je_arm_exception()` remains `_Noreturn`;
- all non-copy slot-4 faults retain the existing fatal/double-fault behavior;
- only a dedicated slot-4 branch may call the ARM usercopy predicate;
- recovery requires exact vector, EL1h mode, EC/IL, expected DFSC, ISS safety bits, ELR, FAR, direction, scope, and owner;
- nested recovery must preserve the outer `je_arm_in_handler` state;
- the x30-preserving restore path must be separately verified.

Option B—generalizing `je_arm_exception()` to return—has a larger blast radius and needlessly changes the established fatal handler contract.

## D6 — proof requirements

The positive test should:

- copy bytes across a page boundary in both directions;
- verify exact destination bytes;
- exercise SELF, QUERY, RIGHTS, DUP with attenuation, CLOSE, and stale-handle rejection;
- verify zero result on failures and `DENIED` for rights inspection after duplicating with zero rights.

The dev injection must separately create:

- level-3 translation fault;
- permission fault at the exact store site;
- fault at a nearby non-approved instruction;
- owner mismatch;
- direction mismatch.

Because normal prevalidation rejects absent/RO pages before assembly, permission and owner/direction cases require deliberate test-only raw-scope injection.

Each mutant needs its own ARM build/smoke profile and must terminate with a clear fatal marker. A single combined boot cannot prove later negative stages after the first intentional halt. The test must also distinguish the intended nested-copy fatal path from ordinary EL0 process death.

Overall: the architecture is viable, but D1/D2/D4 need fixes before approval, and D5 requires explicit owner sign-off because the current slot-4 guard and restore seam make this a genuine frozen-path change.
