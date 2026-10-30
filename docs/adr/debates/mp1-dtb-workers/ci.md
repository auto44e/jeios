# Worker notes — issue #52, W4 remote CI

Observation time: 2026-10-29T23:32:41+00:00  
Repository: JeiOS  
Branch: `wip/52-ci`  
Inspected commit: `a55c2ffe47a1362b62ca6dc821d820265f23f88b`

## State

- Implemented: `.github/workflows/ci.yml` adds an independent `aarch64` job on
  `ubuntu-24.04`, installs the requested ARM64 toolchain/QEMU packages, records
  commit/tool versions, rejects QEMU versions outside 8.2.x, runs the five
  requested ARM build/smoke commands as separate named steps, prints Image
  hashes after each smoke, and uploads `build/aarch64*/serial.log` with
  `if: always()` as `aarch64-serial-logs`.
- Tested: local YAML parsing, whitespace validation, and `make check` passed.
- Planned: lead integration and observation of the GitHub-hosted ARM64 job.
- Blocked/unverified: GitHub Actions cannot be run locally; remote package
  installation, the Ubuntu 24.04 QEMU version, ARM64 boot/vector/DTB/BSS
  behavior, serial-log artifact retention, and remote logs were not observed.

## Exact local verification

- `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml'))"`
  — exit 0.
- `git diff --check` — exit 0.
- `make check` — exit 0. Hosted tests, ELF checks, and ABI/dev-injection
  isolation checks passed; the existing hosted FDT test reports
  `fdt: PLACEHOLDER (suite not yet written)`.
- `git status --short` — only `.github/workflows/ci.yml` is modified;
  this notes file is intentionally uncommitted and outside the requested
  committed file scope.

## Not run

No network access, package installation, `make aarch64`, ARM smoke command,
GitHub Actions workflow run, or artifact upload was performed locally.

## Deviations

None. The x86 `bootstrap` job and its steps were not changed, workflow
permissions remain `contents: read`, and no commit, push, or Git configuration
change was made.
