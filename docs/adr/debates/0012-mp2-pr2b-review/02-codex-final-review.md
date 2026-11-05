Recommendation: request changes before approval. The live path is structurally sound, but two frozen-contract gaps remain.

Findings, ranked:

1. **P1 — Canary proof does not satisfy ADR 0012 D8 for x29/x30.**  
   `CANARY_CHECK` uses x30 as its temporary ([vectors.S:267](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:267)); the loop reloads x29/x30 before comparison ([vectors.S:356](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:356), [vectors.S:395](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:395)); and the final pass checks only x0–x28 ([vectors.S:418](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:418)). A corrupted x30 can therefore pass. This is honestly documented, and x16–x18 are genuinely covered, but it is still a deviation from the frozen requirement that includes x30. Owner approval or a proof redesign is required.

2. **P1 — Unexpected non-spurious INTIDs do not fatal in the IRQ wrapper.**  
   GIC code EOIRs and returns `JE_ERR_INVALID` ([gic.c:318](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/gic.c:318)), but `je_arm_irq()` merely increments a counter and returns ([main.c:1117](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/main.c:1117)). ADR 0012 D6 requires EOIR followed by FATAL. In the default build this can silently resume execution; in the test build failure is only detected later.

3. **P1/P2 — The new proof is not enforced by CI.**  
   `.github/workflows/ci.yml` has no `ARM_TIMER_TEST=1` smoke step and does not run `tools/test_smoke_arm_timer.py` ([ci.yml:56](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/.github/workflows/ci.yml:56)). `make check` also omits the oracle. Current evidence is local only.

4. **P2 — The smoke oracle cannot independently prove ISTATUS==1.**  
   It validates vector, ISR_EL1, IAR, SPSR, ELR, timing, heartbeat and BRK, but ISTATUS remains an implementation dependency in `gic.c:325`. PR-2a hosted tests cover this, so this is acceptable only if explicitly recorded as cross-PR coverage.

Assembly review positives:

- Slots 0–4 and 6–15 are byte-identical; the frozen common saver/double-fault prefix is also byte-identical.
- x16–x19 spill, frozen offsets, EL1h check, `add sp,#304`, full GPR restore, and `eret` are correct.
- The transient guard is set before `bl je_arm_irq` and cleared only after return ([vectors.S:212](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:212), [vectors.S:224](/home/ANT.AMAZON.COM/lubuel/oss/jeios-workspace/jeios/kernel/arch/aarch64/vectors.S:224)); a synchronous fault inside the C handler reaches the fatal double-fault path.
- The final I-masked x0–x28 pass correctly closes the fourth-tick gap and does cover x16–x18.

Observed locally: `make check`, focused `ARM_TIMER_TEST` QEMU smoke, and the 27-mutation oracle all passed. Remote CI and physical ARM remain unobserved.
