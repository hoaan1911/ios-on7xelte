# 2026-11-09 — J7 G3D: verify AS0 before submit, after every map (0327)

- The 0326 image booted to Apple logo, then black, exactly as before 0326: the log is from
  that run (boot-args identical to the usual set; `launchd` prints
  `... sweetfb_scale=1 sweetfb_lw=750 sweetfb_lh=1334` appended, so `iphone8=true` did add the
  internal-screen sizing). The 0326 code is on the phone: `fault repair:` lines appear with
  `bo=... va=... fixed=16/95/4 first=... unmappable=0 tables=32..39/512`.
- What 0326 settled: every reported fault is `ours=0` and the repaired pages are a **whole
  BO prefix** (`fixed=95` = 96 pages minus the faulting page, so the entire scanned prefix was
  absent). The L2 entry is 0 outright on the `300b040` fault (`l2_entry=0 l3_table=0`), i.e.
  the L3 table for that 2 MiB region did not exist yet even though the BO had been recorded
  as mapped. So the holes are in the driver's own tables — not a stale GPU TLB view, not a
  wrong pointer.
- What 0326 changed in the failure mode: the fault is now handled (`fault at 3010180 ... ours=0`
  then `fault repair: ... fixed=4`, `mmu_irq` cleared, poll resumes) but the submit then reports
  `status=-1` (timeout) instead of the old `-3`: clearing the MMU fault does not restart a job
  slot that is already in fault state (`js=8`), so the chain never completes, Mesa still drops
  the batch (`panfrost_batch_submit failed: 5`), and `hakios-gl` still reports
  `1 picture copied out, 0 drawn in place` with `a copy` attaches. Repairing at fault time
  cannot win that race; the holes have to be filled before the walker runs.
- The display side is not the blocker: `SWEET FB: swap 1 layer 0 surface 2 750x1334 ... bpr 3072`
  is copied by `present()` (`A5XELTE FB: frames=1 ... bytes=8294400` = one full 1080x1920
  panel copy), and `bytes=0` on the later window is correct — that swap is surface 1, the
  framebuffer's own surface, which `present()` deliberately skips. Only two swaps happen in
  the whole boot and then `SWEET FB: display off`, because the compositor has no drawn frame
  to present.
- 0327 therefore checks the address space against the BO descriptors *before* it is used:
  - `j7m_verify_bo()` walks a bounded page slice of one BO (rotating per-BO cursor), re-derives
    each page's PA with `j7m_bo_page()` and fills any leaf that is absent or wrong through
    `j7m_map_page_at(..., replace=true)`.
  - `j7m_verify_as(J7M_VERIFY_BUDGET)` runs from submit (selector 2) before the chain is
    handed to the job manager: 4096 pages per submit spread over every BO, round-robin start,
    flush per changed BO, and logs
    `submit verify: checked=... filled=... unknown=... tables=.../...` for the first 24 times a
    hole is found (and once every 120 submits afterwards as a heartbeat).
  - `j7m_map_verify()` re-walks a BO immediately after it is mapped (kernel and client paths),
    fills anything that did not read back, and logs `map verify: bo=... gva=... pages=...
    missing=... unknown=...` — `missing>0` here means the map path itself leaves holes, which
    is the one thing the previous logs could not distinguish.
  - `j7m_bo_free()` now only clears a leaf that still maps that BO's own page; a leaf that maps
    something else belongs to another live BO, and clearing it would unmask that BO for the GPU.
    Cross-BO clearing is logged as `free bo=... kept=... leaves that map another BO`.
  - Selector 0 and 1 log the first 24 `client map gva=... bo=...` / `client unmap gva=...
    ret=...` calls, so BO churn (a VA freed and re-used under a live chain) is visible.
- What the next log decides: `map verify: missing=0` everywhere plus
  `submit verify: filled=0` means the tables are coherent and the remaining fault source is
  GPU-side; `filled>0` on the first submits means entries are being lost between map and
  submit, and the `free ... kept=` / `client unmap` lines say whether frees are the cause.
# 2026-11-09 — J7 G3D: fault-time page-table repair and resume (0326)

- 0325 reached the phone (boot log contains `page-table readback` lines; the image was built
  from xnu run 42497091572, head `c08651e`, and the three j7-boot runs that consumed it
  succeeded). Reading that log end to end corrects the 0325 interpretation: the 0323 walk
  prints `ent` (root at 39), `l2` (level 1 at 30), `l3` (level 2 at 21) and `leaf` (level 3
  at 12), so **the L3 table lives in `ent[2]`, not `ent[1]`** — 0325 read it one level too
  high, which is why its `l3_leaf=0` carried no information.
- What the walk does show for Mesa's real faults: `fault at 3d45000 in bo 18` walks
  `l2=833c4003 l3=833c5003 leaf=0` — an L2 entry that points at a real L3 table
  (`833c5000`) whose leaf for the faulting page is 0; `fault at 38e2440 in bo 11` walks
  `l3=0` outright. Both are leaves the map path had already written (all of bo 4..19 are in
  the `recent m` ring), so a mapped BO is being walked as unmapped. Every one of those
  faults ends `submit jc=... failed status=-3 js=8`, then `panfrost_batch_submit failed: 5`,
  then `hakios-gl ... 1 picture copied out, 0 drawn in place` and an `a copy` attach for the
  750x1334 BGRA screen surface (textures 1, 4 and 7) — i.e. the lock-screen surface falls
  back to a CPU copy of an empty frame.
- The kernel-side selftest still passes in the same boot (`round 0 (good)`, deliberate
  `round 1 (fault)` at `7ff00000`, `round 2 (good)`), so 4-level tables, leaves, job
  submission and completion all work; only the many-BO userland path faults.
- 0326 treats such a fault as a mapping to finish rather than the end of the frame, which is
  the model Panfrost implements in `panfrost_mmu_map_fault_addr`:
  - `j7m_lookup()` walks our own tables the way the walker does (bounds-checked through
    `j7m_in_pool()`), and `j7m_bo_page()` re-derives a page's PA from the BO's descriptor.
  - `j7m_map_page_at(va, pa, replace)` splits the old `j7m_map_page`: normal maps still
    refuse an existing leaf, the repair path rewrites the leaf and replaces an intermediate
    entry that does not point into the pool.
  - `j7m_repair()` is called from `j7m_wait()` on a translation fault: it repairs the
    faulting page plus up to 96 pages of that BO, logs
    `fault repair: bo=... va=... fixed=... first=... unmappable=...`, and returns whether
    the tables changed. When they did, the MMU fault is cleared and the poll loop resumes,
    so the stalled job retries the access instead of failing the submit (8 resumes per job).
    A fault on a VA that is not in any BO (the selftest's deliberate fault) behaves exactly
    as before and still returns -3.
  - `j7m_mmu_flush()` now issues `AS_COMMAND_UNLOCK` (3) after LOCK/FLUSH_PT; the unlock was
    missing, which left the AS locked on the flushed range after every map.
  - The fault readback now reads the L3 table from `ent[2]`, prints the leaf index and adds
    `ours=<0|1> pa=...` from `j7m_lookup()`, so the next log answers directly whether our
    own tables cover the VA the GPU faulted on.
- Boot diagnosis for the next run: if `ours=1` on a fault, the tables already cover the
  page and the remaining problem is on the GPU side (AS0 root, flush/TLB, job slot);
  `ours=0` with `fixed>0` means the mapping really was absent and the repair path took over;
  `ours=0` with `fixed=0` means the page could not be re-derived from the BO descriptor.
- 0326 validation: single-file diff of `iokit/Kernel/arm/A5XELTEGPU.cpp`, generated from a
  tree rebuilt by replaying every patch that touches the file (0125, 0126, 0127, 0128, 0226,
  0227, 0302, 0306, 0307, 0308, 0309, 0310, 0311, 0314, 0318, 0319, 0322, 0323, 0324,
  0325), `index 25a00b4..a1b5205`, `git apply --check` clean against that post-0325 base.

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
