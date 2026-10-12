# Primary references and provenance

Reviewed for architecture planning on 2026-10-12. These sources explain existing
systems or hardware; they do not prove JeiOS implements their behavior. All JeiOS
code in the initial scaffold was authored for this project; no vendor/kernel
source, logos, UI assets or firmware was imported.

| Source | URL | Relevant design area and limit |
| --- | --- | --- |
| Arm future POE2/vMTE architecture discussion | https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/future-architecture-technologies-poe2-and-vmte | Intra-process spatial/temporal permission controls; architectural description is not retail hardware availability |
| Arm permission indirection/overlay documentation | https://developer.arm.com/documentation/102376/0200/Permission-indirection-and-permission-overlay-extensions | Page permission model; exact register semantics must be checked during porting |
| Arm MTE guide discussion | https://developer.arm.com/community/arm-community-blogs/b/operating-systems-blog/posts/new-mte-user-guide | Memory tagging detects classes of memory misuse; not a complete sandbox |
| Arm confidential computing | https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/arm-and-confidential-computing | RME/Realms workload trust boundary; requires platform/monitor support |
| Linux XSTATE documentation | https://docs.kernel.org/arch/x86/xstate.html | Extended-state OS enablement and sizing; not a license to import implementation |
| GrapheneOS features | https://grapheneos.org/features | Attack surface, mitigations, scoped access and diagnostics; Android foundations differ from JeiOS |
| CalyxOS feature list | https://calyxos.org/features/list/ | User-facing privacy/network policy and device boot support; no inherited phone compatibility |
| GNU Multiboot2 specification | https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html | Header, entry and information-tag format; implementation must validate mapped sizes |
| Intel software developer manuals | https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html | Required authoritative register/exception/extended-state reference for future x86 work |
| AMD developer resources | https://www.amd.com/en/developer/resources.html | AMD64 architecture and errata; select exact manual revision during implementation |
| Microsoft kernel-mode driver architecture | https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/ | NT executive, requests, lifecycle inspiration; no WDM/KMDF compatibility claim |
| FreeBSD architecture handbook | https://docs.freebsd.org/en/books/arch-handbook/ | Device/VM architecture inspiration; no BSD kernel merge or ABI implementation |

Detailed implementation changes must cite specific architecture/manual sections
and revision dates in their ADRs or code comments. The broad manuals above are
reference entry points, not evidence that every planned detail was researched
or validated against every CPU stepping. Recheck features and platform support
before implementing any optional extension.
