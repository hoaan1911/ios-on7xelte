## 2026-11-09 — J7 G3D: repair the mapping on fault and resume (0326)

- The 0325 boot log (kernel built from run 42497091572, head `c08651e`) carries the
  `page-table readback` lines, so 0325 did reach the phone. It also shows 0325 read the
  wrong level: the 0323 walk prints root/L1/L2/leaf as `ent`/`l2`/`l3`/`leaf`, so the L3
  table is `ent[2]`, and `ent[1]` (what 0325 read) is the L1 entry pointing at the L2 table.
- Real faults from Mesa: `3d45000` → `l3=833c5003 leaf=0` (L3 table present, leaf absent),
  `38e2440` → `l3=0` (no L2 entry). Both VAs belong to BOs listed in the `recent m` ring, so
  a mapped BO is walked as unmapped. Each fault produces `status=-3 js=8`,
  `panfrost_batch_submit failed: 5` and an `a copy` attach for the 750x1334 screen surface.
  The same boot's kernel selftest passes (round 0 good, deliberate round 1 fault, round 2
  good), so tables, leaves and job completion work for a single kernel BO.
- 0326 implements Panfrost's `panfrost_mmu_map_fault_addr` model: on a translation fault,
  walk our own tables (`j7m_lookup`), re-derive the page PA from the BO descriptor
  (`j7m_bo_page`), force-map the leaf and up to 96 pages of that BO (`j7m_map_page_at` with
  `replace`), flush, clear the MMU fault and **resume the stalled job** instead of failing
  the submit (8 resumes per job, `j7m_repair` from `j7m_wait`). Also adds the missing
  `AS_COMMAND_UNLOCK` to `j7m_mmu_flush` and makes the fault readback read `ent[2]` and log
  `ours=`/`pa=` from our own lookup.

## 2026-11-08 — J7 G3D fault: 0324 refuted, 0325 page-table readback

- New boot log refutes 0324: the `client submit refused: AS0/MMU not ready yet` line and
  the `mmuReady` guard never fire, because AS0 is already up and the 512-table pool is live
  before Mesa opens the user client and submits. The first fault is the kernel-side
  deliberate fault round, not a status=-3/js=8 client submit. 0324 is kept as a refuted
  hypothesis; the guard is harmless but not causal for the persistent `status=-3 js=8`.
- Narrowed failure: a page-table/mapping visibility gap, not submit timing. The BO is
  mapped and its L0/L1/L2 chain exists, but the L3 table/leaf for the fault VA is missing
  when the GPU walks it (e.g. VA `38e2440`: `l2=833e5003 l3=0 leaf=0`).
- Patch 0325 extends the 0323 fault walk: when the walk finds a valid L2 entry
  (`ent[1]`), read back from our own pool the L3 table base it points to and whether that
  L3 table has a valid leaf for the fault VA, logging `l2_entry=... l3_table=... l3_leaf=...`.
  Mirrors the donor HeroGL read-fault path (hgu_pte) but reads from our pool via
  `j7m_table`. Distinguishes "mapping present but GPU still faults" from "L2 allocated, L3
  leaf never written/lost" — the latter is what the current log shows.

## 2026-11-07 — J7 G3D: refuse client submit until AS0/MMU ready (0324, refuted)

- Hypothesis tested and refuted by the 2026-11-08 boot log: guard selector-2 submit with
  `kIOReturnNotReady` until `gJ7M.mmuReady` is set after `j7m_serve_start` programs AS0
  and brings up the page-table pool, to prevent a too-early client submit causing
  `status=-3 js=8`.
- Result: the guard never triggers on the phone — AS0/pool are already ready when Mesa
  submits — so 0324 does not explain the fault. Retained in the tree only as a documented,
  refuted attempt; not the path forward.
