# Working on the HEROLTE port (Galaxy S7 SM-G930F, Exynos 8890)

- Read `HEROLTE_PROGRESS.md` and `docs/herolte-bringup.md` first; record new device evidence in the former. The A5XELTE files below are the reference implementation this port adapts.
- XNU trials may write only BOOT (`/dev/block/platform/155a0000.ufs/by-name/BOOT`). Keep the stock BOOT and a TWRP backup of EFS (IMEI) off the phone; never write EFS, RECOVERY or the bootloader partitions. Confirm every other partition write with the user first.

# Working on the A5XELTE fork

- For Samsung SM-A510F work, read `A5XELTE_PROGRESS.md` and `docs/a5xelte-migration-map.md` first. Record new device evidence in the former and commit completed changes. Keep private VM addresses, passwords, device dumps and downloaded Apple binaries out of git.
- Preserve recovery p11, EFS p3 and the existing Linux BOOT checkpoint. Current XNU trials may write only the explicitly authorized BOOT p10; the donor's Xiaomi `fastboot` and USERDATA instructions are not A5XELTE instructions.
- Keep build success, recovered kernel markers, physical screen reports and iOS runtime behavior as separate claims. `NOTES.md` below is SWEET donor history; do not edit it to imply Samsung behavior.

## Original SWEET contributor instructions (donor reference)

- Read `NOTES.md` before starting; it contains the project goal, hardware facts,
  current blocker, local artifact locations and the next concrete action.
- The user switches agents frequently. Update the active handoff in `NOTES.md`
  after each milestone and before stopping. Record commits, CI run IDs, exact
  build/boot results and unfinished work. Distinguish observations from guesses.
- Commit completed changes. Use `gh` for GitHub Actions kernel builds and logs.
  Local single-file cross-compiles are useful before spending a CI run.
- Device bring-up uses temporary `fastboot boot` images. Preserve the Android
  installation and partition backups. The user approved (2026-10-15) using
  internal UFS for iOS and said Android may be deleted: the iOS root goes
  into the existing `userdata` partition (no GPT changes). Keep bootloaders,
  recovery, modem, persist and the backups intact, and confirm each
  destructive step with the user before running it.
- Keep device dumps, serial numbers and downloaded Apple artifacts out of git.
