# Build issues report — ADprobe (WSL / host)

This document records problems encountered while compiling and linking this project under **WSL (Linux userland)** with **g++** and **C++14**, what caused them, and how they were addressed.

---

## 1. Missing Adaptive AUTOSAR libraries at link time

**Symptom**

```
/usr/bin/ld: cannot find -lara_someipbinding
/usr/bin/ld: cannot find -lsomeip
... (similar for dlt, ara_ipcbinding, ara_exec, ara_com, ara_log, ara_per)
```

**Cause**

The original `Makefile` always linked against vendor / Yocto-style libraries that are present on the target ECU or in a full SDK, but are **not installed** in a typical WSL Ubuntu environment.

**Fix**

- Introduced **`HOST_BUILD`**, defaulting to **`1`**.
- When `HOST_BUILD=1`, link with **`-pthread` only** (no `ara_*` / `someip` / `dlt`).
- When integrating on a real stack, build with **`make HOST_BUILD=0`** and ensure `LIBRARY_PATH` / `PKG_CONFIG_PATH` (or equivalent) point at the Adaptive stack.

**Files**

- `Makefile` — conditional `LDFLAGS` and source list.
- `gedr_stub.cpp` — see issue 2.

---

## 2. Undefined reference to `probe::GEDR_Write`

**Symptom**

```
undefined reference to `probe::GEDR_Write(unsigned int, unsigned char const*, unsigned int, bool, bool)'
```

**Cause**

`ProbeComm.cpp` declares `GEDR_Write` inside `namespace probe` as an **extern** integration hook. With `HOST_BUILD=1`, no GEDR library is linked, so the symbol is never defined.

**Fix**

Added **`gedr_stub.cpp`**, which defines `probe::GEDR_Write` to return success (`0`) and do nothing. This is appropriate for **compile/link verification and dry runs** on a host; replace with the real GEDR provider on target.

**Files**

- `gedr_stub.cpp` (new)
- `Makefile` — includes `gedr_stub.cpp` when `HOST_BUILD=1`.

---

## 3. Incomplete types in `ProbeApp.cpp` (`ProbeCommVariant`, `CanConsumer`)

**Symptom**

Errors such as invalid use of incomplete type when calling methods on `probeCommVariant_` or `canConsumer_` from the implementation file, even if forward declarations existed in the header.

**Cause**

Forward declarations in `ProbeApp.hpp` are enough for **pointers** in the header, but the **`.cpp` file** that calls member functions needs the **full class definitions** (via includes).

**Fix**

Added:

- `#include "ProbeCommVariant/ProbeCommVariant.hpp"`
- `#include "CanConsumer/CanConsumer.hpp"`

to `ProbeApp/ProbeApp.cpp` (after `ProbeApp.hpp`).

**Files**

- `ProbeApp/ProbeApp.cpp`

---

## 4. Missing `InitiateSomeIpDiscovery` on `ProbeComm`

**Symptom**

`ProbeApp` (or generated code) called `probeComm_->InitiateSomeIpDiscovery()`, but **`ProbeComm` had no such member**; the documented discovery entry was `FindServiceDaqContinual()` (void, async callback).

**Cause**

Naming mismatch between application wiring and the actual `ProbeComm` API surface.

**Fix**

- Declared **`bool InitiateSomeIpDiscovery() noexcept`** in `ProbeComm.hpp`.
- Implemented it in `ProbeComm.cpp` to call **`FindServiceDaqContinual()`** and return the current value of **`daqCommunicationEstablished_`**.

Note: discovery is still driven by the availability callback; the return value reflects state **after** registration (and any synchronous callback behavior in the stub layer).

**Files**

- `ProbeComm/ProbeComm.hpp`
- `ProbeComm/ProbeComm.cpp`

---

## 5. Wrong use of `.load()` on `daqCommunicationEstablished_`

**Symptom**

```
error: request for member 'load' in '... daqCommunicationEstablished_', which is of non-class type 'bool'
```

**Cause**

`InitiateSomeIpDiscovery` initially returned `daqCommunicationEstablished_.load()` as if the member were `std::atomic<bool>`, but in `ProbeComm.hpp` it is a plain **`bool`**.

**Fix**

Return **`daqCommunicationEstablished_`** directly.

**Files**

- `ProbeComm/ProbeComm.cpp`

---

## 6. Type mismatch: `SendContinualShortDataCyclic100ms` / `SendContinualLongDataCyclic1000ms`

**Symptom**

Passing `std::vector<std::vector<uint8_t>>` (steady-state staging buffers) into APIs that expect **`std::vector<uint8_t>`**.

**Cause**

Staging structure used nested vectors; the send APIs expect a **flat** byte payload.

**Fix**

Before each send, **flatten** the nested buffer: concatenate each inner `std::vector<uint8_t>` into a single `std::vector<uint8_t>` and pass that to the send function.

**Files**

- `ProbeApp/ProbeApp.cpp` (`SendRegularData` path)

---

## 7. Corrupted / invalid generated C++ (`null` tokens, broken strings)

**Symptom** (historical; fixed in bulk in the codebase)

- Tokens like `null` where valid C++ was required (`std::vector<uint8_t>null`, `idnull`, broken identifiers).
- String literals split across lines without proper concatenation or termination.

**Cause**

Damage or incorrect transformation of generated or merged sources (e.g. automated replace, merge conflict markers interpreted as code, or a broken generator pass).

**Fix**

- Systematic repair of identifiers and string literals (including helper scripts where used).
- Restore valid C++ syntax so the translation units compile.

**Files**

- Multiple under `ProbeComm/`, `ProbeCommVariant/`, `CanConsumer/`, `ProbeApp/` (as applicable in your tree).

---

## 8. `main.cpp` / `ProbeApp` construction mismatch

**Symptom** (historical)

`ProbeApp` constructed with a wrong argument type (e.g. a runtime handle) instead of the real dependencies.

**Cause**

Stub or outdated `main` did not match `ProbeApp(ProbeComm*, ProbeCommVariant*, CanConsumer*)`.

**Fix**

Wire **`ProbeComm`**, **`ProbeCommVariant`**, **`CanConsumer`**, and required buffers, then call **`HandleInitialize()`** and **`Run()`** as intended.

**Files**

- `main.cpp`
- `ProbeApp/ProbeApp.hpp` / `ProbeApp.cpp` (as wired)

---

## 9. Runtime notes after a successful host build

When running `./ADprobe` under WSL without shared-memory / config assets, logs may show:

- `ReadVariantCode: shared memory read failed`
- `SetVariant: variant dictionary load failed at path: config/variant_dictionary.cfg`

These are **environment / data path** issues, not compiler errors. Provide a valid `config/variant_dictionary.cfg` and the expected IPC/shm setup for full behavior.

---

## Build commands (summary)

| Goal | Command |
|------|---------|
| Default WSL / host build | `make` |
| Debug | `make BUILD_TYPE=debug` |
| Link against full Adaptive stack | `make HOST_BUILD=0` (with libs on linker path) |
| Clean | `make clean` |

---

*Generated for the dummy AD probe trial project; adjust `HOST_BUILD` and stubs when moving to production integration.*
