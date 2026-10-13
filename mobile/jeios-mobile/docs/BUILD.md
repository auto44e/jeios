# Build and Pixel qualification plan

Status: not built. Sources reviewed 2026-10-13. No AOSP checkout or binaries were
downloaded; no license acceptance, firmware installation or device operation ran.

## Target selection

Use Cuttlefish for the first current-AOSP framework experiment. Select an explicit
upstream tag and inspect its product/release definitions before setting the lunch
string. Record build host/tool versions, source manifest and artifact hashes.
Do not use a moving latest-release branch as reproducibility evidence.

First physical candidate is base Pixel 9, codename tokay. Google's reference lists
Android 15 tag `android-15.0.0_r32` for build `BP1A.250505.005`, and its driver page
lists a corresponding Pixel 9 vendor image. This is a historical feasibility
anchor, not the selected daily-use release or evidence that we built it. Device
sources, kernel and release config still need inspection and compatible revisions.
Pixel 10, frankel, is the next candidate; a codename or stock factory image alone
does not establish an available AOSP device tree, redistributable blobs or a lunch
product. Record each variant separately; Pro/XL/Fold models are not aliases.

Do not extrapolate older Pixel recipes to current Android releases. Google's AAOS
Pixel guidance states Android 16+ phone builds are unsupported there because
vendor images are no longer shared. That is a warning about that documented path,
not proof that every community mobile build is impossible. Current device-tree,
vendor extraction/provenance and maintenance feasibility remain explicit gates.
Do not choose obsolete security patches merely to obtain a convenient recipe.

## Required build lock before first image

For each target record immutable manifest revision and resolved repo manifest;
exact product definition path; release config; userdebug variant for lab use;
kernel/GKI and module revisions; vendor/firmware build IDs; hashes; provenance and
license terms; host requirements; commands; expected output and verification.
Google vendor downloads have their own restrictions: do not commit or redistribute
them automatically. Community sources also require revision/license review.

The documented modern command shape is:

```sh
source build/envsetup.sh
lunch <verified-product>-<verified-release-config>-userdebug
m
```

This is explanatory syntax, not a runnable scaffold command. Do not substitute
`aosp_tokay` or `jeios_tokay` until the checked-out tree actually defines it. New
product overlays, AndroidProducts.mk and Android.bp modules are added only after
an unchanged upstream target builds and boots. The seed deliberately contains no
fictional device tree or manifest claiming ready-to-build support.

## Gates

MB0: source/provenance inventory, host capacity check, immutable source lock and
verified Cuttlefish product. MB1: unchanged upstream build/boot, logs and repeatable
smoke tests. MB2: minimal original JeiOS service/UI integration and protocol tests.
MB3: Pixel 9 source/vendor/kernel matching, device recovery plan, build then a
separately authorized lab flash and hardware matrix. MB4: signed update/recovery,
SELinux/permission review and parity tests. Pixel 10 gets its own MB3 evidence.

Hardware qualification covers boot/AVB, touch/display, Wi-Fi/Bluetooth/NFC/USB,
security keys, keystore, audio/camera, cellular/modem, power/suspend, thermal load,
GPU/NPU, external displays and OTA rollback. Emulator PASS cannot qualify these.
Document failures explicitly. Preserve recovery images under applicable licenses
without putting blobs or signing keys in this repository.

## Primary references

- https://source.android.com/docs/setup/build/building
- https://source.android.com/docs/setup/reference/build-numbers
- https://developers.google.com/android/drivers
- https://source.android.com/docs/setup/build/building-pixel-kernels
- https://source.android.com/docs/automotive/start/pixelxl

Pin and recheck exact source/device guidance when MB0 starts.
