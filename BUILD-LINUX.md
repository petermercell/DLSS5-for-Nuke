# DLSS5Live on Linux — build, run, and what it cost to get here

**Status: working.** DLSS-NR (NGX feature 18) and DLSS-SR (feature 1) both
evaluate and produce real pixels on Linux, through Wine, on an NVIDIA RTX card.
No Windows machine is needed at any point — not to build, not to run.

Confirmed on:

| | |
| --- | --- |
| OS | Rocky Linux 9.8, kernel 5.14.0-687 |
| GPU | NVIDIA RTX A5000 (GA102, `sm_86`) |
| Driver | 610.57.04 |
| Nuke | 17.0v1 and 17.1v1 |
| Wine | 11.16 Staging, Kron4ek `amd64-wow64` |
| vkd3d-proton | 3.0.1 |
| DXVK | 3.1 |
| dxvk-nvapi | 0.9.2 |
| Compiler | mingw-w64 GCC (worker), system GCC (node) |

---

## What this port is

`DLSS5Live` is two programs, and they are different problems.

**The Nuke node** is genuinely cross-platform now. `WorkerBridge` has a POSIX
implementation (`fork` + `execvp` + anonymous pipes) beside the Win32 one, the
SRW-lock and condition-variable use maps onto pthreads, and module-relative path
discovery goes through `dladdr` instead of `GetModuleFileName`. The wire protocol
is byte-identical, so a Linux node and a Windows worker understand each other
exactly as before.

**The worker cannot be made native.** It evaluates NGX feature 18, which is not
in NVIDIA's public DLSS SDK in any form — the public SDK ships Linux Vulkan
binaries for Super Resolution and Ray Reconstruction only, and no
`nvngx_dlssnr.so` exists. The worker is D3D12 throughout and depends on a caller
shim that satisfies the runtime's return-address module check. So it stays a
Windows PE and runs under Wine, with vkd3d-proton translating D3D12 to Vulkan.

Both halves cross-compile on Linux. The shim's `CALL`/`RET` frames survive
mingw-w64 GCC correctly (`call *%rax` → volatile store → `ret`, no tail-call
`JMP`), and `build_worker_linux.sh` verifies that with objdump on every build.

---

## The one thing to read before you change any NGX code

This cost days, so it goes first.

**MSVC and GCC order overloaded virtual functions in a vtable differently.**
MSVC gathers every overload of a name at the position of the *first*
declaration and emits them in **reverse** declaration order. GCC — and therefore
mingw-w64 — emits them in declaration order. `_nvngx.dll` is built with MSVC.

Transcribe NVIDIA's `NVSDK_NGX_Parameter` from the public header into a mingw
build and you get a mirror-imaged vtable inside each overload group:

```
Set(name, ID3D12Resource*)   ->  actually calls  Set(name, float)
```

which reads its argument from XMM2, never sees the pointer, and stores nothing.
NGX then fails `EvaluateFeature` with `0xBAD00005` and logs
`could not find Color parameter`.

What makes this vicious is that **it half-works**. `Set(unsigned int)` lands on
`Set(int)`, and 32 bits are 32 bits — so `Width`, `Height`, `OutWidth`,
`OutHeight` and the create flags all arrive perfectly. `CreateFeature` succeeds,
reports the correct dimensions, allocates 38 MB of tensors, and looks completely
healthy. Only the resource pointers vanish, silently.

The interface must therefore be declared with each overload group reversed when
not building with MSVC. Both `worker/NGX.h` and `worker/DLSSWorker.h` carry the
declaration and **both must stay in step** — fixing one and not the other
produces exactly the same symptom as fixing neither.

Verified slot assignment against `_nvngx.dll` from driver 610.57.04:

| slot | | slot | |
| --- | --- | --- | --- |
| 0 | `Set(void*)` | 8 | `Get(void**)` |
| 1 | `Set(ID3D12Resource*)` | 9 | `Get(ID3D12Resource**)` |
| 2 | `Set(ID3D11Resource*)` | 10 | `Get(ID3D11Resource**)` |
| 3 | `Set(int)` | 11 | `Get(int*)` |
| 4 | `Set(unsigned int)` | 12 | `Get(unsigned int*)` |
| 5 | `Set(double)` | 13 | `Get(double*)` |
| 6 | `Set(float)` | 14 | `Get(float*)` |
| 7 | `Set(unsigned long long)` | 15 | `Get(unsigned long long*)` |

Slot 16 is `Reset()`. The real vtable has 18 entries — slot 17 is something newer
than the public header knows about, and is not needed.

**If you ever suspect a parameter isn't reaching NGX, read it straight back**
with the matching `Get` and compare. `DLSS5_DUMP_SR=1` does this for the whole
SR parameter set. A value that doesn't survive `Set` → `Get` never reached NGX,
and that single check is what finally cracked this after three wrong theories.

---

## 0. Prerequisites

```bash
sudo dnf install epel-release
sudo dnf config-manager --set-enabled crb
sudo dnf install mingw64-gcc-c++ cmake ninja-build gcc-c++ zstd
```

`zstd` matters — vkd3d-proton ships `.tar.zst` and GNU tar shells out to it.

### SELinux

Wine needs executable-modify permission or it cannot load `ntdll.dll` at all.
The failure appears in the SELinux alert browser as `wine-preloader / execmod`.

```bash
sudo setsebool -P selinuxuser_execmod 1
sudo setsebool -P selinuxuser_execstack 1
```

`sudo setenforce 0` proves the diagnosis but reverts on reboot — use the
booleans for a permanent fix.

### Wine — do not use the distro package

EPEL's Wine cannot be installed on current Rocky at all:

```
nothing provides mesa-libOSMesa(x86-64) needed by wine-core-8.0-1.el9
```

Rocky 9.8 dropped `mesa-libOSMesa` from CRB, and pulling the 9.7 package back
fails too, because current `mesa-libGL` *obsoletes* it. The dependency is
genuinely unsatisfiable without downgrading mesa. Don't. EPEL's Wine is 8.0
(January 2023) anyway — older than the vkd3d-proton and dxvk-nvapi builds this
needs.

```bash
./tools/setup_wine_portable.sh
```

That fetches the current [Kron4ek](https://github.com/Kron4ek/Wine-Builds)
`staging-amd64-wow64` tarball into `~/.local/share/dlss5-wine`, checks its
library dependencies, and proves it starts. The `wow64` variant runs 64-bit
Windows binaries with no 32-bit host libraries, which suits both EL9's multilib
gaps and this project — the worker is pure x64.

Pass `--version X.Y` if the GitHub API rate-limits the lookup, `--variant
vanilla` to skip the Staging patchset.

You do **not** need to export anything: the launcher finds a portable Wine under
`~/.local/share/dlss5-wine/wine-*/bin/wine` by itself, newest version first.
`NUKE_DLSS5_WINE` still overrides it if you want a specific build.

---

## 1. The Wine prefix

```bash
./tools/setup_wine_prefix.sh runtime/
```

Creates the prefix at `~/.local/share/dlss5-wine/prefix` and installs
vkd3d-proton (D3D12 → Vulkan), DXVK (`dxgi`, `d3d11`) and dxvk-nvapi
(`nvapi64`). Read the report it prints at the end — all four DLLs must be
listed `ok`.

Three things about this that are not obvious:

**DXVK is mandatory even though nothing here draws with D3D11.** dxvk-nvapi gets
its Vulkan entry point by querying the DXGI factory, and Wine's builtin `dxgi`
cannot answer that. Without DXVK's `dxgi.dll`, `NvAPI_Initialize` fails and NGX
has no driver behind it.

**The prefix lives outside `~/.nuke` on purpose.** Nuke walks its plug-in path
directories at startup, and a Wine prefix is tens of thousands of files with
DLLs among them — parking one inside `~/.nuke/DLSS5Live` breaks Nuke startup.
It also means uninstalling the plug-in would destroy a 500 MB Wine environment.

**Never run `wineboot -u` on an established prefix.** It restores Wine's builtin
DLLs over `system32`, silently wiping the native vkd3d-proton / DXVK /
dxvk-nvapi installs. The only symptom is `D3D12CreateDevice` failing much later,
which reads like a GPU problem. The launcher now refuses to start rather than
proceed with an incomplete prefix.

### The NVIDIA runtime

`setup_wine_prefix.sh` locates the driver's `_nvngx.dll` (usually
`/usr/lib64/nvidia/wine/`) and copies it into `runtime/`. If it isn't found, the
driver's Wine/NGX component isn't installed:

```bash
dnf provides '*/wine/_nvngx.dll'
```

It goes beside the worker rather than into `system32` deliberately: the worker's
`LoadCoreNGX()` looks in its own directory first, which sidesteps a basename
collision between this project's `nvngx.dll` caller shim and the driver's own
`nvngx.dll`.

Then place your DLSS runtime in `runtime/` — `nvngx_dlssnr.dll` for feature 18,
and `nvngx_dlss.dll` for the SR pass. **Nothing in this repository redistributes
those.**

---

## 2. Build the worker

```bash
./tools/build_worker_linux.sh runtime/
```

Produces `runtime/DLSS_Nuke_Worker.exe`, `runtime/nvngx.dll` (the caller shim)
and `runtime/dlss5-worker.sh`. The script dumps the shim's five `DLSSNR_Call*`
exports and verifies with objdump that each thunk kept a real indirect call and
return — the entire reason the shim exists. **Do not add `-flto`** when
rebuilding it; a tail-call optimisation there turns into `0xBAD00002`.

This step is independent of Nuke and only needs redoing when worker sources
change.

---

## 3. Build the node

```bash
./tools/build_linux.sh /opt/Nuke17.0v1 /opt/Nuke17.1v1
```

Output:

```
bin/Nuke17.0/DLSS5Live.so
bin/Nuke17.1/DLSS5Live.so
```

Per-minor folders are deliberate — Nuke's plug-in ABI moves between 17.0 and
17.1, and `install/init.py` prefers `bin/Nuke<major>.<minor>`, falling back to
`bin/Nuke<major>`. Loading a 17.0 build into 17.1 gives
`undefined symbol: _ZNK2DD5Image2Op12nodeFullPathEc` and takes Nuke down at
startup, because Nuke loads plug-ins to register their node Descriptions before
the UI appears.

If the link fails with a wall of `undefined reference to ... std::__cxx11 ...`,
that Nuke wants the old string ABI:

```bash
NUKE_CXX11_ABI=0 ./tools/build_linux.sh /opt/Nuke17.0v1
```

The `undefined symbol: PyInstanceMethod_Type` lines the script prints at the end
are expected — that's `ldd -r` walking into Nuke's own
`libopenassetio-python.so`, whose Python symbols are resolved by the interpreter
Nuke embeds at runtime. Nothing to do with this plug-in.

---

## 4. Install

```bash
./install/install.sh
```

Creates `~/.nuke/DLSS5Live/`, copies every `bin/Nuke*` build and the runtime, and
registers the plug-in path in `~/.nuke/init.py`. Re-run it after any rebuild.

---

## 5. Verify, in order

Each rung isolates one layer. Climb them in order and a failure tells you
exactly where you are.

### 5a. The bridge, with no GPU

```bash
g++ -std=c++17 -O1 -o /tmp/fake_worker tests/fake_worker.cpp
g++ -std=c++17 -O1 -o /tmp/bridge_test tests/bridge_test.cpp \
    src/WorkerBridge_posix.cpp src/Platform.cpp -Isrc -lpthread -ldl
/tmp/bridge_test /tmp/fake_worker
```

16 checks: handshake, eight sequential frames staying in sync, a missing worker,
a worker that exits at startup, and a worker killed mid-session — which must
fail the frame, not take Nuke down with `SIGPIPE`.

### 5b. NGX itself, with no worker and no Nuke

```bash
cd upstream && bash build_repro.sh
```

`ngx_repro.exe` is a self-contained ~500-line console program using only official
NVIDIA binaries. It creates a D3D12 device, initialises NGX, creates DLSS SR,
uploads a real test image, evaluates, executes the command list, waits on a
fence, and reads the output pixels back. It also dumps the parameter object's
vtable and checks each overload landed on the slot it should.

Expect `RESULT: PASS` and `>>> real pixels came out.`

This is the reference implementation. If the worker misbehaves and this doesn't,
diff them.

### 5c. The full chain, without a Nuke licence

```bash
g++ -std=c++17 -O1 -o /tmp/worker_probe tests/worker_probe.cpp \
    src/WorkerBridge_posix.cpp src/Platform.cpp -Isrc -lpthread -ldl
/tmp/worker_probe ~/.nuke/DLSS5Live/runtime/dlss5-worker.sh --scale 2.0
```

Drives the real worker through the real bridge with a synthetic HDR frame.

| Result | Meaning |
| --- | --- |
| `PASS` | Everything works. |
| No valid `SetupResponse` | Worker died before or during init — read `dlss5-worker.log`. |
| `setup_result = 0xBAD00002` | NR runtime rejected the calling module — the shim's return-address check. |
| Fails at stage 2 | Init fine, Evaluate refused. Parameter problem — see `DLSS5_DUMP_SR=1`. |
| Buffer all zero | Reported success, produced nothing. |

**`--scale 2.0` matters.** At the default scale 1.0 the SR pass is skipped
entirely (`m_needUpscale` is false), so `--scale 1.0` combined with
`DLSS5_SKIP_NR=1` is a no-op that returns an untouched black buffer and looks
like a failure.

### 5d. Nuke

```bash
/opt/Nuke17.1v1/Nuke17.1
```

Read or Checkerboard → Tab → `DLSS5Live` → Mode: Single Frame, Motion Vector
Source: None, Upscaling Mode: 2.0x. First evaluation takes a few seconds while
NGX compiles 79 cubin shaders.

Judge the picture, not the return code — compare against a plain Reformat to 2×.

---

## Adding a new Nuke version

This is the only part you repeat regularly. The worker, the shim, the Wine
prefix and the runtime are all Nuke-independent — **none of them need
rebuilding.** Only the `.so` does.

```bash
cd ~/Documents/DLSS5/DLSS5-for-Nuke-linux
./tools/build_linux.sh /opt/Nuke17.2v1     # the new install
./install/install.sh
```

That adds `bin/Nuke17.2/DLSS5Live.so` beside the existing builds and installs it.
`init.py` routes each Nuke to its own build automatically, so old versions keep
working.

Then sanity-check the ABI actually matches, because a bad `.so` prevents Nuke
starting rather than just failing to load:

```bash
/opt/Nuke17.2v1/Nuke17.2
```

If Nuke won't start, move the plug-in aside to confirm it's the cause:

```bash
mv ~/.nuke/DLSS5Live ~/.nuke/DLSS5Live.off
```

Then check whether the symbol it named actually exists in that Nuke:

```bash
nm -D --defined-only /opt/Nuke17.2v1/libDDImage.so | grep <symbol>
```

Present means it's a routing problem (wrong build being loaded); absent means the
headers and library disagree and the build needs `NUKE_CXX11_ABI=0` or a
different NDK path.

If a future Nuke moves to a new compiler or string ABI, only step 3 changes.

---

## When it doesn't work

The launcher writes `~/.nuke/DLSS5Live/runtime/dlss5-worker.log`. Start there.
`stdin`/`stdout` carry binary protocol, so nothing may print to them — every
diagnostic goes to that log.

It's noisy with `libEGL` / `kmsro` warnings from the Mesa loader, which are
harmless:

```bash
grep -vE "libEGL|kmsro|pci id|^$" ~/.nuke/DLSS5Live/runtime/dlss5-worker.log | tail -40
```

### Diagnostic environment variables

| Variable | Effect |
| --- | --- |
| `DLSS5_NGX_VERBOSE=1` | Worker prints its NGX stages and result codes |
| `DLSS5_DUMP_SR=1` | Reads every SR parameter back before Evaluate, and reports which vtable slot each overload compiled to |
| `DLSS5_SKIP_NR=1` | Skips feature 18 entirely — snippet and shim left unloaded, core called directly. Needs `--scale 2.0` to do anything |
| `DLSS5_SR_MINIMAL=1` | Drops every SR parameter the reproducer doesn't set |
| `DLSS5_NGX_FORCE_INITEXT=1` | Uses `Init_Ext` instead of `Init_ProjectID` |
| `DLSS5_KEEP_LD_ENV=1` | Don't scrub `LD_LIBRARY_PATH` / `LD_PRELOAD` before exec'ing Wine |
| `DLSS5_ALLOW_BUILTIN_D3D12=1` | Let the reproducer fall back to Wine's builtin d3d12 |

### Failure signatures

**`D3D12CreateDevice(FEATURE_LEVEL_12_0) failed on the NVIDIA adapter`** — the
prefix is missing vkd3d-proton, or `DXVK_ENABLE_NVAPI=1` isn't set. Without that
variable DXVK reports NVIDIA GPUs with AMD's vendor id (`0x1002`), so the
worker's `0x10DE` filter rejects the only usable adapter. The launcher exports
it; if you're running Wine by hand, you must too.

**`0xBAD00005` at Evaluate** — `FAIL_InvalidParameter`. Almost always a
parameter that never arrived. Run with `DLSS5_DUMP_SR=1` and look for a value
that doesn't read back. Two known causes: the vtable ordering bug above, and
creating the SR feature without the AutoExposure flag — DLSS then expects an
`ExposureTexture` at evaluate time and refuses when it isn't supplied. The create
flags must be `IsHDR | DepthInverted | AutoExposure` = `0x49`.

**`0xBAD00002`** — `FAIL_PlatformError`. The NR runtime rejected the caller's
return address, or the core has no usable driver behind it. Not
version-dependent, so retrying other API versions in a loop only buries the
cause under a later crash.

**`0xBAD0000C`** — `FAIL_OutOfDate`; NGX thinks a component is too old.

**`wine: command not found` in the log** — a Wine that isn't there. The launcher
now searches for one itself and reports every location it tried.

**Nuke won't start at all** — a `.so` built against a different Nuke. See
"Adding a new Nuke version".

**`page fault on execute access to 0x...`** — a null or bogus function pointer.
`0x2` specifically means a wrong `NGXLoggingInfo` layout: the callback field
comes **first**, with no `UserData`, and getting it wrong makes NGX call the
logging-level constant as a function.

**`Init_ProjectID` crashing inside `_nvngx.dll`** — the snippet and core disagree
about the order of the last two arguments (`featureInfo` / `sdkVersion`). The
worker passes `nullptr` there deliberately. Don't "fix" it.

### Two things that look like bugs and are not

**`NvAPI_D3D12_GetCudaIndependentDescriptorObject: Invalid argument`**, five
times per evaluate, for the app-supplied textures. NGX has a fallback path and
produces a full frame of correct pixels regardless. This was chased twice as a
suspected vkd3d-proton bug and is not one — do not file it.

**`vkd3d_texture_view_desc_fixup: Remapping 2D to 2D_ARRAY`** — informational.

---

---

## What this port changes, relative to upstream

For anyone reviewing the fork, or preparing a pull request.

**Modified upstream files (11).** All of these are edits in place; none can be
moved to a separate folder without leaving the originals broken.

| File | Change |
| --- | --- |
| `worker/DLSSWorker.cpp` | Reversed-overload parameter interface use, AutoExposure create flag, per-stage error reporting, NGX log sink, diagnostic env toggles |
| `worker/DLSSWorker.h` | `NGXParameter` overload order guarded by `_MSC_VER` |
| `worker/NGX.h` | Same for `NVSDK_NGX_Parameter`; kept in step with the above |
| `worker/main.cpp` | Reports `getLastError()` and carries `setup_result` across the wire — upstream discarded both |
| `CMakeLists.txt` | Linux node target, `NUKE_INSTALL_DIR`, `NUKE_CXX11_ABI` |
| `worker/CMakeLists.txt` | mingw-w64 cross-compilation |
| `src/WorkerBridge.{h,cpp}` | POSIX branch alongside the Win32 one |
| `src/DLSS5Live.{cpp,h}` | Module-relative worker discovery via `dladdr`, launcher-aware path resolution |
| `install/init.py` | Prefers `bin/Nuke<major>.<minor>` over `bin/Nuke<major>` |

**New files (16).** `src/Platform.{h,cpp}` (Win32→POSIX shims),
`src/WorkerBridge_posix.cpp`, `install/install.sh`, `install/dlss5-worker.sh`
(the Wine launcher), `cmake/toolchain-mingw64.cmake`, four `tools/*.sh`, three
`tests/*.cpp`, `upstream/ngx_repro.cpp` + `build_repro.sh`, and this document.

**Untouched (7).** `worker/caller_shim.cpp`, `worker/Protocol.h`,
`worker/DISOpticalFlow.{cpp,h}`, `worker/build.ps1`, `install/menu.py`,
`.github/workflows/ci.yml`. The Windows build path is unaffected — every
platform-specific change is behind `_WIN32` or `_MSC_VER`.

## Things worth improving

- The worker's `FeatureCommonInfo` struct never produces a log line, so NGX's own
  diagnostics are unavailable from inside the worker. `Init_Ext` is passed
  `nullptr` instead. The reproducer gets NGX logging and the worker doesn't;
  finding out why would make future debugging much faster.
- A Wine crash writes its backtrace to the worker's stdout, which is the protocol
  stream — the probe detects it (`First 4 bytes were 0x656E6957`, "Wine") but a
  clean failure would be better than a corrupted stream.
- `DLSS5_SKIP_NR` skipping at scale 1.0 produces a silently black buffer rather
  than saying "nothing to do".
