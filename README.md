# winecx-gptk

> **this fork.** a fork of [frankea/winecx-gptk](https://github.com/frankea/winecx-gptk), which mirrors [dappermint/winecx-gptk](https://github.com/dappermint/winecx-gptk). the recipe, the workflow and everything below are theirs and unchanged, except that the workflow also runs this fork's probes. the additions:
>
> - [`patches/0002`](patches/0002-ntdll-expose-the-stack-base-at-gs-0x8-on-macOS-on-request.patch): with `WINE_EXPOSE_STACK_BASE=1`, the stack base sits at `gs:0x8` while PE code runs, for programs that read it straight off `%gs` (`tests/stackbaseprobe.c`). it follows the fiber patch that `patches/0001` carries, [dappermint/winecx-gptk#25](https://github.com/dappermint/winecx-gptk/pull/25) by @hetthicker.
> - [`patches/0003`](patches/0003-winemac-take-the-host-s-application-icon-before-reading-the-exe-s.patch): winemac takes the host's `WINE_APP_ICON_PATH` before reading the exe's icon.
> - [`patches/0004`](patches/0004-ntdll-expose-the-stack-base-to-ZFGameBrowser.exe-without-being-asked.patch): 0002 is on by default for `ZFGameBrowser.exe`, a browser that other programs start and so never sees a variable set on just one of them.
> - [`patches/0005`](patches/0005-ntdll-skip-the-gsbase-syscall-on-the-way-into-unix-c.patch): the syscall and unix call dispatchers move `gsbase` back to the pthread TSD only after a signal handler moved it away, instead of with a syscall on every entry, about 160 ns a call under rosetta. `tests/gsbaseprobe.c`, run with `WINE_EXPOSE_STACK_BASE=1`, fails when they get it wrong.
> - [`patches/0006`](patches/0006-ntdll-go-back-to-_sigtramp-when-Rosetta-reports-a-fa.patch): a thread is no longer aborted when rosetta reports a fault that coincides with a suspend with the suspend signal's context (`tests/gsbaseprobe.c`).
> - [`patches/0007`](patches/0007-ntdll-read-the-performance-counter-without-a-syscall.patch): `QueryPerformanceCounter` reads `rdtsc` itself under rosetta, where it counts the same nanoseconds as `mach_continuous_time`. a wine-private flag in `KUSER_SHARED_DATA.Spare` says so; the Windows `Qpc*` fields stay unset (`tests/qpcprobe.c`).
>
> the patches change wine, so they carry wine's licence, LGPL 2.1 or later; wine itself is the work of the wine authors and codeweavers (see `AUTHORS`, `COPYING.LIB` and `LICENSE` in the [winecx tree](https://github.com/dappermint/winecx)). this repository has no licence file of its own upstream, and this fork does not add one. it publishes no runtime release itself; [Dunn-Kim/Whisky](https://github.com/Dunn-Kim/Whisky) ships a hosted build of the `expose-stack-base` branch as its engine. apple's game porting toolkit is not included.

ci build of a gptk-capable wine runtime for the [frankea/Whisky](https://github.com/frankea/Whisky) fork: codeweavers' crossover 26.3 wine changes, rebased onto upstream wine 11.19.

why: apple's game porting toolkit / d3dmetal payload only executes on crossover-derived wine builds, it patches their unixcall internals at load time. details in [frankea/Whisky#163](https://github.com/frankea/Whisky/issues/163), importer app-side in [frankea/Whisky#164](https://github.com/frankea/Whisky/pull/164).

the source tree is the [`wine1119` branch of dappermint/winecx](https://github.com/dappermint/winecx/tree/wine1119): crossover 26.3's diff (221 files against its wine 11.0 base) merged onto wine 11.15 via a synthetic three-way, then carried to 11.16, 11.17 and 11.19 the same way, with every local patch committed in the tree itself. `patches/` in this repo is empty on purpose; the apply step is a guarded no-op. the crossover diff turned out compact enough that tracking upstream wine releases is sustainable, most of the rebase churn was mechanical.

what runs on it, measured on an m5: steam's ui end to end, d3d12 through d3dmetal at feature level 12_2 (binding tier 3, sm 6.6), dxvk d3d11, msync, and the media stack.

an experimental, opt-in lane bundles kosmickrisp as an alternative vulkan driver for upstream dxvk and native vulkan games, see [docs/kosmickrisp.md](docs/kosmickrisp.md). off by default and never published.

what the workflow does:

- clones winecx at the pinned commit, builds the unix half for x86_64 under rosetta
- PE half via mingw-w64 gcc (not llvm-mingw: an llvm-built `kernelbase.dll` stalls steam's CM login, found by module bisection), `--enable-archs=i386,x86_64`
- freetype, gnutls, gstreamer and friends come from pinned nixpkgs x86_64-darwin and are bundled flat into `Wine/lib` with `@loader_path` rewrites, so the tree relocates; moltenvk from khronos' own release
- wine-mono and wine-gecko go in extracted, the same form and the same place the stock whisky engine puts them
- packages a whisky `Libraries.tar.gz` with `gptkCapable` set in the version plist

gates that refuse to ship a bad tree, each one added after that exact thing shipped silently:

- **relocatability.** every Mach-O file is swept for absolute non-system references. a dylib whose own id is a `/nix/store` path cannot be dlopened off the builder, which cost fonts, vulkan and tls for seven builds without a single error message.
- **every bundled library dlopens.** the closure is loaded file by file on the builder with the store paths masked; this caught the libiconv split (`_iconv` vs `_libiconv`) that had silently killed the whole media stack.
- **the runtime opens a window and media foundation has decoders.** `wine --version` passes on runtimes that cannot create a window.
- **the i386 half is non-empty.** a 64-bit-only tree cannot load `syswow64\ntdll.dll`, so every 32-bit program dies with `c0000135`.
- **the PE half is stripped.** gcc emits DWARF and nothing removes it; `ntdll.dll` is 3.0MB unstripped against the stock engine's 0.7MB, and everything still runs.

## supported macos versions

this runtime targets the current macos major release and its immediate predecessor: macos 27 and 26, with a deployment floor of 26.0. older versions are outside this release's support scope. this policy applies to this runtime, not automatically to frankea/Whisky.

release acceptance must cover both supported versions, including Steam Store and popups with and without an imported GPTK payload. CI's window-opening check alone does not establish game compatibility.

## reproducibility

everything the build consumes is pinned in-tree:

| input | pinned by |
|---|---|
| winecx sources | `WINECX_COMMIT` in the workflow env |
| nixpkgs | `NIXPKGS_REV` in the workflow env, a rev not a branch |
| moltenvk, dxvk, dxmt | version + sha256 in the workflow |
| wine-mono, wine-gecko | sha256 table in the workflow, checked after download; the versions are read out of winecx's `dlls/appwiz.cpl/addons.c` and the build stops if an unpinned version appears |

builds run on a self-hosted runner by default (warm ccache, ~15 min); the `hosted` dispatch input is the clean-room check and what releases should come from when provenance matters more than turnaround.

## notable changes carried in the tree

**ntdll: don't call a foreign personality routine as an SEH handler.** `virtual_unwind()` leaves `LDR_DATA_TABLE_ENTRY *module` uninitialised and `LdrFindEntryForAddress` does not touch it on failure, so for a fault in Mach-O code the "personality routine in system library" guard reads garbage and never fires; wine then calls a libunwind personality routine as a windows exception handler and recurses to stack death. still present upstream as of wine 11.16, applied at the moved location there.

**winemac: host cross-process metal layers over CAContext.** wine 11.x grew CALayerHost cross-process swapchains upstream; what it still lacks is child windows and win32 state mirroring for hosted layers, which is what steam's chromium needs. the gpu process renders into another process's child windows; the owner hosts the published tree and re-derives hidden state and z-order from the win32 windows on every WindowPosChanged. without the mirroring, chromium's hidden standby surface covers the live one with one stale black frame, which shows up as a fully rendered steam library under a black layer.

**d3dkmt: adapter identity and segment sizes.** five more KMTQAITYPEs answered honestly from vulkan (adapter type, physical adapter count, pci address, adapter guid, segment sizes), placed above crossover's WDDM 2.7 hack so its fallthrough keeps reaching `default` for non-d3dmetal backends.

**upstream fixes taken ahead of their release.** the 11.15 lane carried eight commits from wine master as cherry-picks; all eight landed in 11.16 and were dropped at that rebase. the one currently carried is the winegstreamer non-fixed-caps video pool fix, which is not in 11.16.

## what happens when rosetta goes

this runtime is x86_64. it runs on apple silicon through rosetta 2, which apple
has said is largely discontinued in macOS 28. everything here has that shelf
life, and so does the apple payload it exists to host: d3dmetal ships x86_64
only, so the d3d12 work, the video interposers and the metalfx bridges cannot
follow the runtime to arm64 unless apple builds them for it.

the successor is an arm64 runtime running x86 games under FEX, and it already
works: our own FEX build executes both 64-bit and 32-bit x86 windows code on an
arm64 mac. what it cannot do is host that process by itself. wine needs the low
4GB of address space, arm64 reserves all of it as a mandatory pagezero, and the
only way out is `com.apple.developer.cross-architecture-support`, an entitlement
apple grants at its own discretion. crossover's arm64 build carries it. a
self-signed binary carrying the same string is refused at exec.

so the ceiling on this project is not engineering. anyone can write the code and
we have. whether it reaches users is apple's decision, and worth knowing before
you build a library around any of this.

## history

the wine 10 line (crossover 25.1, series 4.3) proved d3dmetal executes on a self-built runtime and carried the first version of the cross-process bridge as four patches; the wine 11.0 line (crossover 26.3) collapsed them to one. both era tips are tagged, [`lane/wine10-cx25`](../../tree/lane/wine10-cx25) and [`lane/wine11.0-cx26`](../../tree/lane/wine11.0-cx26), and the old `patches/` files are browsable in those trees.
