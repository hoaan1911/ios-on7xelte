# 2026-11-08 — J7 G3D fault page-table readback (0325)

- New boot log shows 0324's "submit too early / mmuReady" guard never fires: AS0 is
  up and the 512-table page-table pool is live before Mesa opens the user client and
  submits, so `client submit refused: AS0/MMU not ready yet` never appears and the first
  fault is the kernel-side deliberate fault round, not a status=-3/js=8 client submit.
  0324 is therefore the wrong diagnosis for the persistent `status=-3 js=8` MMU fault and
  is retained only as a refuted hypothesis (the guard is harmless but not causal).
- Root cause now narrowed to a page-table/mapping visibility gap, not submit timing: the
  job-chain BO is mapped and its L0/L1/L2 chain exists, but the L3 table/leaf for the
  fault VA is missing when the GPU walks it. First (deliberate) fault walks cleanly
  (`l2=0 l3=0 leaf=0`), then Mesa's real faults show `l2=833e5003 l3=0 leaf=0` for VA
  `38e2440` — an L2 entry that exists but points at nothing usable for that VA.
- Patch 0325 extends the 0323 fault walk so that, whenever the walk finds a valid L2
  entry (`ent[1]` present), it also reads back from our own pool what that L2 entry
  points at: the L3 table base and whether that L3 table actually has a valid leaf for
  the fault VA, logging `l2_entry=... l3_table=... l3_leaf=...`. This mirrors the donor
  HeroGL read-fault path (hgu_pte: read back the PTE for the faulting VA from the
  driver's own page tables) but reads from our pool directly via `j7m_table`.
- Combined with the 0323 walk, 0325 separates "L2 exists, L3 table present, L3 leaf
  present" (the mapping exists in our pool but the GPU still faults — wrong root / AS0 /
  flush / job-slot issue to chase next) from "L2 exists, L3 table present, L3 leaf ZERO"
  (we allocated the L2 table but never wrote, or lost, the L3 table or its leaf for this
  VA — the pattern the current boot log shows). The readback runs in the fault path only
  and does not change submit or mapping behavior.
- Next: get 0325 onto `main` and build/boot it, then re-pull the J7 Prime log (note: the
  J7 log pull must target `SYSTEM mmcblk0p21`, not the A5XELTE `p20` the existing pull
  scripts assume) and read the new `page-table readback` lines against the faulting VA.
