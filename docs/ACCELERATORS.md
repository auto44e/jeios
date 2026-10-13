# Accelerators, compute offloading and media integration

Status: owner requirements and proposed architecture, 2026-10-13. No GPU/NPU
or media runtime is implemented. M3a remains the next kernel gate.

## Cross-platform GPU/NPU architecture

Owner requirement: GPU and NPU support across all planned JeiOS platforms, using open standards by default. Interpret this as a common architecture and API across AMD64, AArch64 and future ports, qualified separately for each device/driver/firmware combination. Publish a matrix of supported operations, precision, memory limits, isolation, reset behavior and test evidence. A platform without an accelerator remains usable through supported CPU implementations; a request requiring an unavailable device or guarantee returns an explicit unsupported result.

Recommended interfaces:

| Layer | Default direction | Qualification boundary |
| --- | --- | --- |
| GPU graphics/compute | Vulkan and applicable SPIR-V profiles | Requires a native driver/runtime and tested feature subset |
| Portable compute | OpenCL where implemented; optional SYCL developer tooling | SYCL is C++ userspace tooling; kernel/core stays C11 |
| NPU inference | ONNX model interchange with a replaceable inference runtime and device backends | ONNX is not a universal NPU driver; operator, shape, precision and compiler support vary |
| Video engines | Explicit decode/encode capabilities; evaluate Vulkan Video where available | Video codec engines are distinct from GPU shaders and NPUs |

Prefer open implementations and documented interfaces. Vendor-specific adapters may supplement the common API when necessary, with explicit licensing and platform limits; they must not become an invisible mandatory dependency. API support on Windows/Linux/macOS does not automatically supply a driver for a fresh JeiOS kernel. Standards versions and conformance claims must be pinned at implementation time.

Keep model compilers, media codecs and scheduling policy in isolated userspace services. Kernel responsibilities remain scoped handles, memory ownership, protected submissions, synchronization, accounting, interrupts and qualified DMA isolation. No arbitrary physical-address submission. Validate command buffers and model inputs, bound compilation time/memory, and isolate compiler failures. Required device containment fails closed if IOMMU/reset prerequisites are unavailable. Device reset must not silently corrupt unrelated workloads or release buffers while DMA can still touch them.

## Smart offloading with conservative defaults

Owner requirement: automatically use GPU/NPU resources for suitable CPU work when resources allow, with full configuration and API control. Implement this through participating libraries, task graphs or explicitly supplied equivalent backend implementations. Do not promise transparent migration of arbitrary CPU instructions. GPU data-parallel kernels and NPU-supported inference operators are eligible; control-heavy code and unsupported operations remain on CPU.

Proposed C-facing API families: enumerate capabilities; create a policy; submit a typed task graph with buffer handles; query selected backend and reason; wait/cancel; collect bounded performance statistics. Names and layouts remain unfrozen. Per-job options should include AUTO/CPU_ONLY/PREFER_DEVICE/REQUIRE_DEVICE, allowed devices, deadline, memory/queue budget, power preference, precision/error tolerance, determinism, fallback permission and data sensitivity. Applications may narrow their grant; policy-changing calls require appropriate handles and cannot override system isolation or another user's resource allocation.

Conservative AUTO defaults:

- Select only validated operation/backend pairs with an equivalent CPU path. No implicit precision reduction, model quantization or output-quality change.
- Include queue delay, compilation/warmup, transfers, synchronization and result-copy cost in the decision. Require a measured benefit margin and use hysteresis to avoid oscillation; exact thresholds follow device measurements.
- Protect desktop interaction, audio deadlines and foreground work. Bound background utilization, memory and queue depth. Respect battery/thermal constraints; do not wake a discrete GPU for trivial work by default.
- Use CPU if suitability or benefit is uncertain. A REQUIRE_DEVICE request returns a clear failure instead of silently falling back. Report the actual backend and fallback reason.
- No cloud offloading by default. Confidential buffers may use only explicitly authorized devices/backends; faster execution never expands access rights.
- Cancellation stops further scheduling but is not proof of device quiescence. Retain buffers until fences/completion or validated reset establish safety. Retry on CPU only for operations declared safe to replay, staging outputs so failures cannot commit twice.

Expose the same settings through API, CLI and eventual GUI, with per-system, per-user, per-application and per-job policy. Higher privilege may allocate budgets, but an advanced UI never bypasses capability enforcement. Local performance feedback is bounded and does not imply telemetry export.

## Built-in media support

Treat “built in” as first-class, maintained OS distribution integration through userspace packages/services and a shared media API. Minimal/headless images may omit packages and report unavailable features. Codec parsing does not belong in the kernel, and integration does not imply every codec or hardware backend exists.

| Component | Integration target | Acceptance conditions |
| --- | --- | --- |
| FFmpeg | Maintained libraries/CLI and isolated media workers; selected software codecs first, qualified hardware paths later | Pinned build configuration, source/license notices, malformed-media tests, correct timestamps/color/audio, resource bounds |
| HandBrake | Maintained transcoding application/CLI using JeiOS storage and accelerator interfaces | Native port and runtime dependencies; tested presets, cancellation, partial output cleanup and a later GUI port |
| Blackmagic RAW | First-class .braw decode, playback, metadata/sidecar and color-processing integration through a versioned adapter | SDK terms and native OS/ISA feasibility reviewed; reference clips validate pixel/color/metadata results; no unsupported encoding promise |

FFmpeg describes its base licensing as LGPL-2.1-or-later, with optional GPL components changing the build's terms. Preserve those licenses and qualify the precise dependency/build configuration. HandBrake distributes GPLv2 license text and remains a separately licensed application; inspect file/dependency notices for the selected release. Neither component becomes Apache-licensed through bundling.

Blackmagic's public SDK page lists macOS, Windows and Linux and CPU/GPU acceleration, including Metal, CUDA and OpenCL. It does not establish a native JeiOS SDK or permission to relicense its implementation. Obtain and review actual SDK terms and dependencies before bundling. Its advertised free availability is not proof of open-source implementation or arbitrary-port rights. Record a native port, a separately qualified compatibility route, or an explicit unavailable status. Do not assume FFmpeg/HandBrake automatically gain BRAW support merely because the OS has an adapter. Cross-component integration requires explicit implementation and licensing review.

Use NPUs for supported inference stages such as an explicitly selected enhancement model. Do not claim an entire FFmpeg/HandBrake pipeline can run on an NPU. Decode, filtering, inference and encoding are separately scheduled stages; quality-changing enhancement is opt-in.

## Project license

Original JeiOS code is Apache-2.0 under the owner's decision; see
[ADR 0005](adr/0005-licensing.md) and [LICENSE](../LICENSE). Dependencies keep
their own terms. No codec, SDK or driver is imported in this change.

## Placement in the roadmap and tests

M3a remains unchanged. M3b/M3c should leave room for future typed DEVICE, BUFFER and FENCE objects without implementing them prematurely. M4 supplies device lifecycle, scoped buffer grants and DMA/reset containment. M5/M6 provide storage, application runtime and initial software media ports; their CPU instruction prerequisites also need qualified state preservation. M7 supplies desktop media integration. M8 qualifies GPU compute/video, NPU backends and automatic workload placement. Software media need not wait for accelerated backends, but must not execute SIMD before its context support exists.

Acceptance must include CPU-reference correctness, precision/quality guarantees, transfer-inclusive benchmarks, battery/thermal behavior, foreground latency under background load, memory/queue exhaustion, malformed models/media, cancel/timeout/reset races, cross-process data isolation and explicit CPU fallback. Each platform earns support through reproducible tests. No accelerator or media milestone is completed by this addendum.

Primary references checked for this addendum: [Vulkan](https://www.vulkan.org/), [OpenCL](https://www.khronos.org/opencl/), [SYCL](https://www.khronos.org/sycl/), [ONNX](https://onnx.ai/), [FFmpeg legal/build guidance](https://ffmpeg.org/legal.html), [HandBrake COPYING](https://github.com/HandBrake/HandBrake/blob/master/COPYING), [Blackmagic RAW and SDK](https://www.blackmagicdesign.com/products/blackmagicraw), [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0). These describe upstream interfaces and terms, not existing JeiOS support.
