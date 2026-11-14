# Xcode workspace for jeios

Open `JeiOS.xcodeproj` on a Mac with Xcode (Xcode 15 or later recommended).
Choose a shared scheme and use **Product > Build / Command-B**. These are external
build targets, not macOS/iOS app targets. Run, Archive and XCTest are not configured;
there is no automatic simulator, signing, flashing or kernel debugger attachment.
Use the report navigator for command output and the existing CLI/QEMU workflows
for execution/debugging. Scheme names: check, aarch64, integration, mutants.

Install/start Docker Desktop (or a compatible Docker CLI/engine) and grant it
access to the checkout. Schemes call the existing `.docker/ci-run.sh` with
`make <task>`, using linux/amd64 for the builder, including on Apple Silicon.
First build needs network access and may be slow under emulation. This is a Linux
build of JeiOS; Apple's SDK/compiler is not substituted for the ELF toolchain.
The runner's isolated mode uses tracked working-tree files; save edits first and
`git add` new source files to include them. Output/build logs follow the existing
container runner; this project does not export container binaries into Xcode.
Do not run multiple schemes against the same checkout concurrently.

The wrapper adds standard Homebrew/Docker CLI locations to PATH because GUI Xcode
may not inherit your terminal environment. Missing dependencies fail visibly.
Paths are relative to the checkout, including locations containing spaces.

The navigator lists tracked source/docs/configuration as normal groups. After
adding/removing files run `python3 tools/generate_xcode.py`, then commit the project
changes. No generated build products or per-user Xcode settings belong in git.
No native compile target is added: navigation/editing is supported, but accurate
compiler-driven indexing/completion is not promised for external Linux builds.

Validation on the authoring Linux host covers plist/XML structure, stable target
IDs, scheme references and task commands. Xcode opening, `xcodebuild -list` and
Command-B on Intel/Apple Silicon Macs still need real Mac validation. On a Mac:

```sh
plutil -lint *.xcodeproj/project.pbxproj
xcodebuild -list -project JeiOS.xcodeproj
```

The projects are developer convenience tooling, not OS/platform qualification.
