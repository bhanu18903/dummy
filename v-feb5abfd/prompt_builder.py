"""
Prompt Builder
Constructs fully detailed prompts for portable C++ code generation.

STRATEGY:
  - All AUTOSAR Adaptive (ara::) runtime calls are REPLACED by a lightweight
    portable C++ runtime abstraction (IRuntime interface + SimRuntime implementation).
  - Application logic, handlers, data processing flow, and function signatures
    are preserved EXACTLY as described in the UDD.
  - Generated code must compile and run without any AUTOSAR SDK.
  - Prompts inject INTEGRATION_BUILD_RULES so codegen avoids common g++/link failures
    (lambda capture, .hpp/.cpp name parity, namespace linkage, stub shape, duplicate statics).
  - IB19+ in INTEGRATION_BUILD_RULES capture WSL/g++ compile-hygiene lessons (strings, ODR, namespaces, stubs).
  - IB38+ capture host/WSL link and composition-root lessons (merged in this file).

Expected project folder structure:

    ProbeComm/
        ProbeComm.hpp         <- #include "../runtime/IRuntime.hpp"
        ProbeComm.cpp
    ProbeCommVariant/
        ProbeCommVariant.hpp
        ProbeCommVariant.cpp
    ProbeApp/
        ProbeApp.hpp
        ProbeApp.cpp
    CanConsumer/
        CanConsumer.hpp
        CanConsumer.cpp
    AdaptiveApplication/
        AdaptiveApplication.hpp
        AdaptiveApplication.cpp
    main.cpp                  <- creates SimRuntime, injects into all components
"""

from typing import Dict, List

from app.autosar_codegen.models.component_model import ComponentModel
from app.autosar_codegen.models.function_model import FunctionModel

# ------------------------------------------------------------------ #
# !! CRITICAL NOTICE — READ BEFORE ALL RULES !!
# ------------------------------------------------------------------ #
AUTOSAR_IGNORE_NOTICE = """

"""

PSEUDOCODE_EXECUTION_GUARANTEE = """
 
=== PSEUDOCODE_EXECUTION_GUARANTEE ===
 
The pseudocode section is the single authoritative source of logic and behavior. Every operation explicitly described in the pseudocode except comments MUST be fully implemented.
No operation may be omitted, altered, simplified, deferred, or commented out. All variable names used in the pseudocode MUST be reused exactly as written in the generated code.
No additional logic may be introduced beyond what is defined in the pseudocode. No stubs, placeholders, TODOs, or commented‑out implementations are permitted.
The output MUST consist solely of valid, standard‑compliant C++ code. Comments are allowed only for explanations, not in place of required functionality.
The generated code must reflect complete end‑to‑end behaviour, ensuring no loss of functional coverage from the pseudocode.
If the pseudocode specifies an external interaction (service discovery, proxy creation, subscription, destruction),you MUST express it as a concrete C++ abstraction call.
Commented‑out or simulated behaviour is strictly forbidden. If an abstraction is missing, YOU MUST DEFINE ONE.
Array, loop, and slot semantics etc.. in the pseudocode MUST be implemented exactly and may not be simplified.
PSEUDO CODE KEYWORDS
1.CALL RULES:
- Any line beginning with CALL represents required execution.
- CALL statements MUST result in executable logic in generated output.
- CALL statements MUST NOT be rendered as comments or descriptive text.
- Absence of arguments does NOT reduce executable intent.
 2. CALLBACK RULES:
- CALLBACK statements represent required callback registration, binding, or invocation.
- CALLBACK statements MUST NOT be rendered as comments.
3. COMMENT SEPARATION RULES
 
Only pseudocode lines explicitly beginning with // may be treated as comments.
// applies only to that single line.
Any pseudocode line not beginning with // MUST be treated as executable logic.
Executable pseudocode lines MUST NOT be converted into:
    -comments
    -block comments
    -descriptive text
    -placeholders

"""
# ------------------------------------------------------------------ #
# Plain C++ + MISRA-inspired coding rules
# ------------------------------------------------------------------ #
CPP_CODING_RULES = """
=== MANDATORY CODING STANDARDS ===

You MUST follow ALL of the following rules without exception.
Generate STANDARD C++ ONLY 

--- C++14 / C++17 Rules ---
1.  Use std::optional<T> for functions that may return a value or nothing.
    Use bool + output parameter, or exceptions for error-returning functions.
    Never use raw error codes without documenting their meaning.
2.  Use std::cout / std::cerr for output, or accept a logger interface via
    constructor injection. Never use printf or AUTOSAR logging APIs.
3.  Use std::thread, std::mutex, std::condition_variable for concurrency.
4.  Use std::future / std::promise / std::async for asynchronous operations.
5.  Use std::unique_ptr / std::shared_ptr for all heap-allocated objects.
    Never use raw new / delete.
6.  Wrap all code in the project namespace provided.
7.  Use #pragma once in ALL header files.
8.  Use fixed-width integer types where bit-width matters:
    uint8_t, uint16_t, uint32_t, uint64_t,
    int8_t,  int16_t,  int32_t,  int64_t  (from <cstdint>).
    Use int / size_t / ptrdiff_t for general-purpose indexing and sizes.
9.  Prefer stack allocation. Minimize heap allocations — use RAII for all
    resources (files, sockets, mutexes, threads).
10. Thread safety: protect all shared data with std::mutex or std::atomic.

--- MISRA C++-inspired Rules ---
M1.  No raw owning pointers. Use std::unique_ptr or std::shared_ptr.
     Non-owning raw pointers (observers) are allowed where lifetime is clear.
M2.  All variables MUST be initialized at the point of declaration.
M3.  const correctness: every parameter/member that is not modified MUST be const.
M4.  No implicit type conversions. Use explicit casts only (static_cast, etc.).
M5.  Every switch statement MUST have a default case.
M6.  No recursion unless depth is strictly bounded and documented.
M7.  Single responsibility: each function does exactly one thing.
M8.  Validate all function parameters before use.
M9.  Mark noexcept on all functions that are guaranteed not to throw.
M10. No C-style casts. Use static_cast, reinterpret_cast only where necessary.
M11. No macros for constants — use constexpr or const instead.
M12. All #include paths must use the exact component folder structure.
M13. No unused variables or parameters (use [[maybe_unused]] if needed).
M14. Boolean expressions must be explicit (no implicit int-to-bool).
M15. No goto statements.

--ISO_Guidlines------
ISO 26262 functional safety requirements (ASIL‑B),
- ISO/IEC 14882 (C++ language standard),

--- File Structure Rules ---
F1.  Header file (.hpp): #pragma once FIRST, then Doxygen @file block,
     then #include directives, then namespace + class declaration.
F2.  CPP file (.cpp): Doxygen @file block at very top BEFORE any #include,
     then #include directives, then implementations.
F3.  Inter-component includes use relative paths:
     #include "../ComponentName/ComponentName.hpp"
F4.  Only standard library headers use angle brackets:
     #include <string>
     #include <vector>
     #include <memory>
     #include <mutex>
     #include <optional>
     DO NOT include any ara:: or AUTOSAR headers — they do not exist here.
"""


# ------------------------------------------------------------------ #
# Complete Doxygen documentation rules
# ------------------------------------------------------------------ #
DOXYGEN_RULES = """
=== MANDATORY DOXYGEN DOCUMENTATION RULES ===

All generated code MUST include comprehensive Doxygen documentation
that passes `doxygen -w` without warnings. Follow ALL rules below:

--- FILE-LEVEL (every .hpp AND .cpp file) ---
D1. For .hpp files: place #pragma once FIRST, then IMMEDIATELY after place
    the Doxygen @file block BEFORE any #include directives.
    For .cpp files: place the Doxygen @file block at the very top BEFORE
    any #include directives.
    The block MUST contain:
      @file    <exact filename with extension>
      @brief   <one-line summary of the file's purpose>
      @details <2-3 sentences explaining the component's role in the system>
      @author  <team or individual name>
      @date    <YYYY-MM-DD creation date>
      @version <semantic version e.g. 1.0.0>
      @copyright Copyright (c) 2024 Company. All rights reserved.

    Example for .hpp:
    /**
     * @file   DataProcessor.hpp
     * @brief  Processes incoming sensor data and dispatches results.
     * @details Receives raw data buffers, applies filtering and calibration,
     *          then forwards processed results to registered consumers.
     *          Thread-safe for concurrent producer/consumer access.
     * @author  Engineering Team
     * @date    2024-01-01
     * @version 1.0.0
     * @copyright Copyright (c) 2024 Company. All rights reserved.
     */

--- NAMESPACE-LEVEL ---
D2. Document EVERY namespace with:
      @namespace <name>
      @brief     <purpose of this namespace>

    Example:
    /// @namespace probe
    /// @brief Contains all components of the probe application.
    namespace probe {

--- MODULE GROUPING ---
D3. Use @defgroup in ONE central header per module and @ingroup in all
    related files to group components logically.

    Example in central header:
    /**
     * @defgroup DataAcquisition Data Acquisition Components
     * @brief Components responsible for collecting and buffering sensor data.
     */

    Example in related file:
    /**
     * @ingroup DataAcquisition
     */

--- CLASS / STRUCT / ENUM LEVEL ---
D4. Every class, struct, and enum MUST have:
      @class / @struct / @enum <Exact_Name> (NO spaces — use underscores)
      @brief    <one-line summary>
      @details  <multi-line: responsibilities, design rationale,
                 thread-safety guarantees, and lifecycle>
      @ingroup  <module group from D3>
      @note     <coding standard compliance notes>
      @warning  <misuse hazards>
      @invariant <class invariants e.g. mutex ownership rules>
      @see      <cross-references to related classes>

D5. Every member variable (public AND private) MUST have:
      @brief <purpose, valid range, units if applicable>

    Example:
    uint32_t retryCount_{0U};  ///< @brief Current retry count. Range: [0, maxRetryCount_].

D6. Every enum value MUST have:
      @brief <meaning and when this value is used>

--- FUNCTION / METHOD LEVEL ---
D7. Every function and method (public, private, free, static) MUST have:
      @brief   <one-line summary starting with a verb>
      @details <multi-line: algorithm/strategy, thread-safety,
                error handling approach, interaction with other components>
      @param[in] / @param[out] / @param[in,out] for EVERY parameter with:
                description, valid range, and units
                NEVER use bare @param without direction qualifier!
      @return  <type and description>
                NEVER write "@return void" — omit @return entirely for void functions
      @retval  <value> <when this value is returned> — one per distinct return value
               Example:
               @retval true  Operation completed successfully
               @retval false Input validation failed or resource unavailable
      @throws  <ExceptionType> <when it is thrown> — or "None" for noexcept functions
      @pre     <precondition(s) the caller must satisfy>
      @post    <guaranteed state after the call>
      @note    <compliance notes, implementation caveats>
      @warning <deadlock risks, blocking behavior, performance implications>
      @see     <related functions or classes>
      @requirements <SWR-REQ-XX-XX-XXX> traceability to software requirements
      @rationale <design decision explanation when approach is non-obvious>

--- CONSTANTS AND MACROS ---
D8. Every constexpr, const, and static variable MUST have:
      @brief   <purpose>
      @details <valid range, units, derivation of magic numbers>

    Example:
    /// @brief Maximum number of retry attempts before giving up.
    /// @details Set to 5 based on the 10-second startup timeout requirement,
    ///          allowing one retry per 2 seconds.
    static constexpr uint8_t kMaxRetryCount{5U};

--- CONSISTENCY RULES ---
D9. Use IDENTICAL formatting across ALL files:
    - Always use @param[in], @param[out], or @param[in,out] — NEVER bare @param
    - Always place @file block BEFORE #include directives
    - Use consistent @copyright string: "Copyright (c) 2024 Company. All rights reserved."
    - Use /** ... */ style for all multi-line Doxygen blocks
    - Use ///< for inline member variable documentation

--- THINGS TO NEVER DO ---
D10. DO NOT:
    - Write "@return void" (just omit @return entirely for void functions)
    - Place @file blocks AFTER #include directives
    - Leave struct/enum members without @brief
    - Use bare @param without direction qualifier [in]/[out]/[in,out]
    - Skip Doxygen on .cpp file internal helpers, structs, enums, or constants
    - Use inconsistent @copyright formats between files
    - Omit @pre/@post on safety-critical functions
    - Omit @retval when multiple return values are possible
    - Use spaces in @class, @struct, or @enum tag names (use underscores)
    - Leave any function, method, variable, or constant undocumented
"""


# ------------------------------------------------------------------ #
# Portable build / link / stub consistency (codegen → g++ / WSL)
# ------------------------------------------------------------------ #
INTEGRATION_BUILD_RULES = """
=== PORTABLE BUILD, LINK, AND STUB CONSISTENCY (MANDATORY) ===

Generated sources must compile and **link** with g++ (C++14 or C++17) in a Linux/WSL or
cross-platform tree. Follow ALL items below in addition to pseudocode.

--- Includes and Makefile ---
IB1. Use **forward slashes** in #include paths and in Makefiles. Backslashes are not path
     separators on Linux/WSL (they produce bogus paths like CanConsumerCanConsumer.cpp).
IB2. Match the repo layout: from a component .cpp, include headers as e.g.
     `#include "CanConsumer/CanConsumer.hpp"` or `#include "../CanConsumer/CanConsumer.hpp"`
     depending on include roots (`-I.`). Do not rely on a non-existent flat `Foo.hpp` next to main.
IB3. Link all translation units that define **extern "C"** or unique stubs (e.g. PlatformStubs.cpp,
     SharedMemStubs.cpp) into the final binary. If the link step omits them, you get undefined references.
IB4. Prefer explicit `$(OBJS)` + `$(LDFLAGS)`; on some toolchains add `-lstdc++` if iostream symbols fail.

--- One spelling everywhere (.hpp ↔ .cpp) ---
IB5. Class **member** names in the header MUST be **byte-identical** to every use in the .cpp.
     Do not rename between files (e.g. `categoryTriggerCountMap_` in .hpp vs `categoryTriggerCountMap_null`
     in .cpp, or `cameraMutex_` vs `cameraMutex_null`).
IB6. **Locals** and **parameters** must match pseudocode/spec exactly. Do not concatenate tokens into invalid
     identifiers (e.g. `selectedHandlenull` instead of `selectedHandle`, or `cameraMutex_nullnull`).
IB7. If the specification uses a deliberate suffix pattern (e.g. `_null` on buffers, or trailing `_` only),
     apply that pattern **consistently** in both .hpp and .cpp. The rule is **consistency**, not banning `_null`.

--- Namespace and linkage ---
IB8. After `namespace <name>` in a .cpp, an opening `{` MUST appear before out-of-line method definitions.
     A missing brace breaks the rest of the translation unit (methods “not declared” in class).
IB9. If call sites use `ns::LogInfo` / `ns::LogError`, the **definitions** MUST live in `namespace ns { ... }`.
     Implementations hidden only in an **anonymous** namespace do **not** produce `ns::LogInfo` at link time.

--- Lambdas ---
IB10. Any automatic variable or `this` member read inside a lambda must be **captured**:
      use `[this, foo]`, `[=]`, or `[&]` as appropriate. `[](...){ ... zatInstanceId ... }` without capture is ill-formed.

--- Stubs: one API, complete shape ---
IB11. Define each helper (e.g. `CreateServiceProxy`, `CreateServiceSkeleton`) **once** with a single signature.
      Every call site must pass argument types that match (e.g. `const ServiceHandle&`, not `uint32_t` where a
      handle struct is required).
IB12. If a stub `struct ServiceProxy` replaces a real proxy, declare **every** member used by callers
      (`created`, `SubscribeEvent`, etc.) and give minimal noexcept bodies so the code compiles.
IB13. Never define the same `static` function or `static constexpr` **name** twice in the same file/namespace.

--- const correctness ---
IB14. Do not assign to non-`mutable` data members inside `const` member functions. If the algorithm updates
      state (`effectiveLogStartTime_`, `cameraAnswerBackReceived_`, …), remove `const` from the method in
      **both** .hpp and .cpp.

--- Cross-component calls ---
IB15. Methods invoked on dependencies (e.g. `variant_->REQUEST_BDP_UPLOAD_REQ_RES(...)`) MUST be declared in
      that dependency’s header with matching types; implement stubs there if the real stack is absent.

--- Dev-log helpers ---
IB16. Avoid two file-scope `BuildLogFileName` (or similar) in different namespaces — ambiguous overload resolution.
      Use one helper or qualify calls.

--- Runtime / visible diagnostics ---
IB17. Stubs for `IRuntime` / `SimRuntime::logInfo` / `logError` should not be empty `{}` if the product owner
      expects console output during bring-up; route to `std::cout` / `std::cerr` when appropriate.

--- Variant sequencing ---
IB18. When pseudocode separates shared-memory read, default fallback, and dictionary feature application, preserve
      that structure (or a single helper that implements the **full** equivalent). Do not drop safety branches
      only to silence compile errors.

--- ADDITIONAL INTEGRATION RULES (compile/link hygiene — mandatory) ---
These items address failures seen when generated C++ was fixed for WSL/g++ without vendor stacks.
They extend the rules above; do not contradict IB1–IB18.

--- String literals and streams ---
IB19. Never split a C++ string literal across physical lines in a way that leaves a dangling fragment
      (e.g. a line ending with `<< "part` and the next line starting with `";`). Each string literal must be
      syntactically complete on one line, or use raw string continuation with closing quote on the same logical
      string, or use `\\n` inside one quoted string, or insert `<< '\\n'` / `<< std::endl` as a separate token.
IB20. After edits, mentally tokenize every `std::cout` / `std::cerr` chain: every `<<` operand must be a valid
      expression; no stray `";` that closes the statement early.

--- Invalid tokens and “null” noise ---
IB21. NEVER emit invalid patterns such as `std::vector<T>null`, `Type null`, or `noexcept null`. Use `{}`,
      default construction, or correct keywords only.
IB22. Do not concatenate the word `null` onto identifiers (`proxynull`, `entrynull`, `resultBuffernull`,
      `matchingIdsnull`, `selectedHandlenull`). If the header uses a deliberate `_null` suffix on a **member**,
      spell it exactly as in the header — do not double suffix (`_nullnull`).
IB23. In constructor initializer lists, use `member{}` or `member()` only for types that require it; do not list
      bare names that are not base classes or members; do not use nonsense aggregate inits for mutex/map types.

--- Includes and incomplete types ---
IB24. Include every standard header you use: e.g. `#include <array>` for `std::array`, `#include <string>` for
      `std::string`, `#include <cstdint>` for fixed-width integers, etc.
IB25. In a .cpp file, `#include` the full component header(s) for any dependency whose **methods** you call,
      not only a forward declaration from another header. Example: if you call `ReadVariantCode()` / `SetVariant()`
      on `ProbeCommVariant`, include `ProbeCommVariant.hpp` in that .cpp even if another header forward-declares the class.

--- One-definition rule and duplicates in one translation unit ---
IB26. Do not define the same `static constexpr` name, `static` free function name, or macro twice in the same
      .cpp / same namespace scope. Merge or rename (this amplifies IB13).
IB27. If two subsystems each need similarly named file-scope helpers (`BuildLogFileName`, `OpenLogFile`, …) in the
      same .cpp, give the second set **distinct names** (e.g. `BuildRcvLogFileName`, `OpenRcvLogFile`) or put them in
      separate named namespaces — otherwise overload/ODR errors follow (amplifies IB16).

--- Namespaces when mixing stubs and “real-shaped” middleware ---
IB28. If one file contains both application types (e.g. `namespace probe`) and a second layer of service/middleware
      stubs, place the second layer in a **named** namespace (e.g. `namespace mw { ... }`) — not a second anonymous
      namespace that collides with types — and qualify calls (`mw::CreateServiceProxy`, `std::vector<mw::ServiceHandle>`).
IB29. Ensure stub structs match call sites: if code uses `.valid`, `.handle`, or specific method names, declare those
      members and signatures on the stub type (amplifies IB12).

--- const correctness vs state ---
IB30. If a method assigns to non-mutable members, clears flags, or updates timestamps, it MUST NOT be `const` in
      either .hpp or .cpp (same rule as IB14; do not “fix” compile errors by const_cast).

--- extern globals and link-time symbols ---
IB31. For every `extern` global or `extern "C"` symbol referenced, provide exactly one matching definition in some
      .cpp in the portable tree, or declare it in a single stub translation unit linked into the binary.
IB32. If pseudocode or headers call a vendor-only API (e.g. persistence, NVM, GEDR) that is not linked in portable
      builds, supply one portable stub implementation with the **exact** name and parameter types callers use,
      returning documented success/error codes — do not leave undefined references.

--- Static members and entry-point wiring ---
IB33. Inside member functions, qualify `static` class members with `ClassName::` when the compiler may not establish
      class scope (e.g. `ClassName::kMaxBytes`, `ClassName::kInvalidTimestamp`).
IB34. `main.cpp` (or the composition root) must construct objects in an order valid for their constructors; use the
      correct namespace for all types; pass `nullptr` only where the API explicitly allows optional dependencies.
IB35. Every member used in implementations MUST be declared in the class (e.g. counters, mutexes, buffers). Do not
      reference undeclared identifiers “to match” pseudocode — add the member to the header with correct type.
IB36. Call only methods that exist on the declared API. If pseudocode names helpers that are not in the dependency
      header, implement them on the owning class or adjust to the real method names (e.g. `SetVariant` vs invented
      `ApplyDefaultVariantCode`) rather than calling non-existent symbols.

--- Standard level ---
IB37. If the project build uses **C++17** (`-std=c++17`), prefer C++17 features only where already required
      (`std::optional`, structured bindings, etc.) and ensure all translation units are consistent with that standard.

--- ADDITIONAL INTEGRATION RULES — HOST / WSL BUILD POST-MORTEM (IB38+) ---

These items **extend** IB1–IB37 above. Follow **all** prior rules; the issues below address failures seen when
linking without a full Adaptive AUTOSAR SDK.

--- Composition root and wiring ---
IB38. **`main.cpp` / composition root**: Instantiate classes using constructors **exactly** as declared in their headers
      (parameter types, order, count). Inject dependencies (`ProbeComm*`, `ProbeCommVariant*`, `CanConsumer*`, etc.)
      in a valid construction order. Do not pass a runtime-only or wrong-type object where a component pointer is
      required. Call lifecycle methods (`HandleInitialize`, `Run`, …) only if they exist on the declared API.

--- Atomicity and member API consistency ---
IB39. Use **`.load()` / `.store()`** only when the member is `std::atomic<T>`. For a plain `bool`, `uint32_t`, etc.,
      read and assign directly. Calling `.load()` on a non-atomic member is ill-formed.

--- Cross-class API surface ---
IB40. Every member function invoked on a dependency (e.g. `probeComm_->X()`) **must** be **declared** in that
      dependency's **`.hpp`** with compatible signature. If pseudocode or UDD uses a **different name** than the
      refactored API (e.g. pseudocode says `InitiateSomeIpDiscovery` but the class exposes `FindServiceDaqContinual`),
      add a **thin wrapper** on the owning class (declaration + definition) instead of emitting calls to
      **non-existent** symbols from the caller.

--- Container types vs. wire/send APIs ---
IB41. **Do not** pass `std::vector<std::vector<uint8_t>>` (or other nested staging) to parameters that expect
      `std::vector<uint8_t>` unless the header explicitly declares that overload. **Either**: flatten inner vectors
      in a defined order before the call, **or**: stage data in a single flat buffer member declared in the **`.hpp`**
      so types match everywhere.

--- Incomplete types in implementation files ---
IB42. **Amplifies IB25.** Any **`.cpp`** that calls **methods** on an injected component must `#include` that
      component's **full header**, not rely only on forward declarations from another header. Typical orchestration
      classes (`ProbeApp.cpp`, etc.) need includes for **every** collaborator whose methods are used.

--- `extern` / vendor integration symbols ---
IB43. For each `extern` function or namespace integration hook (e.g. `probe::GEDR_Write(...)`) referenced from
      generated code, ensure the **link step** provides **exactly one** definition: real vendor object/library **or** a
      **stub** `.cpp` with the **identical** name, namespace, parameter types, and noexcept/calling convention.
      Host/WSL builds often omit Adaptive libraries — stubs must be listed in the Makefile when needed.

--- Makefile / default developer build ---
IB44. Generated or hand-maintained **Makefiles** should document **host vs. target** link: e.g. a **`HOST_BUILD=1`**
      (or equivalent) path that links **`-pthread`** only and adds stub objects for missing `-lara_*` / `-lsomeip` /
      `-ldlt`, versus **`HOST_BUILD=0`** with full Adaptive `LDFLAGS`. Use **forward slashes** in paths (IB1). This
      prevents false "cannot find -lara_…" failures on WSL.

--- Duplicate scaffolding in one translation unit ---
IB45. **Amplifies IB13 / IB26 / IB28.** Do not declare the **same** helper struct/type name twice in one `.cpp`
      (e.g. duplicated `ServiceInstanceId` / `ServiceHandle` in overlapping anonymous namespaces). Unify helpers;
      compare handles using **one** agreed member path (e.g. `handle.instanceId.id`) everywhere.

--- String and token hygiene (regression guard) ---
IB46. **Amplifies IB19–IB22.** Never emit `Type null`, `std::vector<T>null`, doubled suffixes like `_nullnull`, or split
      string literals that leave **unterminated** quotes across lines. After generation, mentally verify every
      `std::cout`/`std::cerr` chain and every identifier against the **`.hpp`**.

--- Runtime assets (non-compile but user-visible) ---
IB47. Log messages that reference **paths** (`config/variant_dictionary.cfg`, shared-memory names) must use paths
      consistent with the **deployed** layout or document required assets. This does not block compilation but avoids
      false "broken build" reports during bring-up.
"""


class PromptBuilder:
    """
    Builds prompts for header (.hpp) and implementation (.cpp) generation.
    Generates STANDARD C++ ONLY — all AUTOSAR/Adaptive AUTOSAR content is
    explicitly suppressed regardless of what the input data contains.
    All content is derived from ComponentModel and FunctionModel data.
    """

    def __init__(self, namespace: str):
        """
        Args:
            namespace: C++ namespace to wrap all generated code in.
                       e.g. "probe" or "feature_x" — from config, not hardcoded.
        """
        self.namespace = namespace

    # ------------------------------------------------------------------ #
    # Public API
    # ------------------------------------------------------------------ #

    def build_header_prompt(
        self,
        component: ComponentModel,
        global_interface_map: Dict[str, List[str]],
    ) -> str:
        sections = [
            self._header_task_intro(component),
            AUTOSAR_IGNORE_NOTICE,
            CPP_CODING_RULES,
            DOXYGEN_RULES,
            INTEGRATION_BUILD_RULES,
            self._component_overview(component),
            self._dependency_signatures(component, global_interface_map),
            self._all_function_signatures(component),
            self._private_members_section(component),
            self._interfaces_section(component),
            self._header_output_instructions(component),
        ]
        return "\n\n".join(s for s in sections if s.strip())

    def build_cpp_prompt(
        self,
        component: ComponentModel,
        global_interface_map: Dict[str, List[str]],
    ) -> str:
        sections = [
            self._cpp_task_intro(component),
            AUTOSAR_IGNORE_NOTICE,
            CPP_CODING_RULES,
            DOXYGEN_RULES,
            INTEGRATION_BUILD_RULES,
            PSEUDOCODE_EXECUTION_GUARANTEE,
            self._component_overview(component),
            self._dependency_signatures(component, global_interface_map),
            self._all_function_implementations(component),
            self._state_machine_image_notice(component),
            self._cpp_output_instructions(component),
        ]
        return "\n\n".join(s for s in sections if s.strip())

    def get_state_machine_images(self, component: ComponentModel) -> List[str]:
        """
        Collect all state machine images across all functions in the component.
        Returns list of (label, base64_png) tuples.
        """
        result = []
        for func in component.functions:
            for idx, img_b64 in enumerate(func.state_machine_images):
                label = (
                    f"State Machine Diagram for function: {func.unit_name}"
                    + (f" (diagram {idx+1})" if len(func.state_machine_images) > 1 else "")
                )
                result.append((label, img_b64))
        return result

    def build_header_prompt_signatures_only(
        self,
        component: ComponentModel,
        global_interface_map: Dict[str, List[str]],
    ) -> str:
        """
        Lightweight header prompt for large components (>10 functions).
        Sends only function SIGNATURES (from Excel) — no pseudocode or DOCX detail.
        This keeps the prompt small regardless of how many functions exist.
        The .hpp produced here becomes the context anchor for all CPP batch calls.
        """
        sections = [
            self._header_task_intro_large(component),
            AUTOSAR_IGNORE_NOTICE,
            CPP_CODING_RULES,
            DOXYGEN_RULES,
            INTEGRATION_BUILD_RULES,
            self._component_overview(component),
            self._dependency_signatures(component, global_interface_map),
            self._all_function_signatures(component),   # signatures only (Excel)
            self._private_members_section(component),
            self._interfaces_section(component),
            self._header_output_instructions(component),
        ]
        return "\n\n".join(s for s in sections if s.strip())

    def build_cpp_batch_prompt(
        self,
        component: ComponentModel,
        batch_functions: List[FunctionModel],
        batch_index: int,
        total_batches: int,
        global_interface_map: Dict[str, List[str]],
        hpp_content: str,
    ) -> str:
        """
        Batch CPP prompt for large components (>10 functions).
        Each call handles only BATCH_SIZE functions at a time.

        Includes:
          - AUTOSAR ignore notice (suppresses any ara:: content in input data)
          - The complete .hpp as context anchor (inter-class + intra-class consistency)
          - Global interface map (inter-class linking — same as single prompt)
          - Full Excel + DOCX data for ONLY the functions in this batch
          - Plain C++ / MISRA / Doxygen rules

        Args:
            component:            Full component (for overview + dependency context)
            batch_functions:      Only the functions in this batch
            batch_index:          Current batch number (1-based)
            total_batches:        Total number of batches for this component
            global_interface_map: All component signatures (inter-class linking)
            hpp_content:          The generated .hpp file content (context anchor)
        """
        sections = [
            self._cpp_batch_task_intro(
                component, batch_functions, batch_index, total_batches
            ),
            AUTOSAR_IGNORE_NOTICE,
            CPP_CODING_RULES,
            DOXYGEN_RULES,
            INTEGRATION_BUILD_RULES,
            PSEUDOCODE_EXECUTION_GUARANTEE,
            self._component_overview(component),
            self._dependency_signatures(component, global_interface_map),
            self._hpp_context_section(component, hpp_content),
            self._batch_function_implementations(component, batch_functions),
            self._cpp_batch_output_instructions(component, batch_functions),
        ]
        return "\n\n".join(s for s in sections if s.strip())

    # ------------------------------------------------------------------ #
    # Header prompt sections
    # ------------------------------------------------------------------ #

    def _header_task_intro_large(self, component: ComponentModel) -> str:
        """Task intro for large component header — signatures only approach."""
        return f"""
=== TASK: Generate Standard C++ Header File (Large Component) ===

You are generating a production-quality STANDARD C++ HEADER FILE (.hpp) not illustrative or explanatory code
for the software component: {component.component_name}

Namespace  : {self.namespace}
File name  : {component.component_name}.hpp
Functions  : {len(component.functions)} (large component — signature-based generation)

This header will be used as the context anchor for all subsequent
implementation generation. It MUST declare ALL {len(component.functions)} functions completely.

IMPORTANT: Generate STANDARD C++ ONLY. 

Requirements:
- Standard C++14 / C++17 
- Complete Doxygen documentation on EVERY element
- #pragma once as the very first line
- Doxygen @file block immediately after #pragma once, BEFORE any #include
- All inter-component dependencies properly included using relative paths
- Class declaration with ALL {len(component.functions)} methods and private members
- Do NOT implement any functions — declarations only
""".strip()

    def _hpp_context_section(self, component: ComponentModel, hpp_content: str) -> str:
        """Inject the generated .hpp as context anchor for batch CPP generation."""
        if not hpp_content:
            return (
                f"=== HEADER CONTEXT ===\n"
                f"Header file for {component.component_name} was generated previously.\n"
                f"Your implementations MUST be consistent with that header.\n"
                f"Include it as: "
                f'#include "{component.component_name}/{component.component_name}.hpp"'
            )
        return (
            f"=== GENERATED HEADER FILE (Context Anchor) ===\n"
            f"The following is the complete generated header for {component.component_name}.\n"
            f"Your implementations MUST be 100% consistent with this header.\n"
            f"Use EXACTLY the same method signatures, types, and member names.\n\n"
            f"{hpp_content}"
        )

    def _cpp_batch_task_intro(
        self,
        component: ComponentModel,
        batch_functions: List[FunctionModel],
        batch_index: int,
        total_batches: int,
    ) -> str:
        """Task intro for a CPP batch call."""
        func_names = ", ".join(f.unit_name for f in batch_functions)
        return f"""
=== TASK: Generate Standard C++ Implementations (Batch {batch_index}/{total_batches}) ===

Component  : {component.component_name}
Namespace  : {self.namespace}
File       : {component.component_name}.cpp
Batch      : {batch_index} of {total_batches}
Functions in this batch ({len(batch_functions)}): {func_names}

You are implementing ONLY the {len(batch_functions)} functions listed above.
The complete header file is provided below as your context anchor.
Your implementations MUST be 100% consistent with that header.

Requirements:
- Implement ONLY the functions listed in this batch — no others
- Use the exact method signatures from the header
- Full Doxygen on every implementation
- Standard C++14 / C++17 — no ara:: types,
- implement full functionality as per the pseudocode in the production level invoke it concretely in the generated code
- Output ONLY the function implementations — no class declaration, no #pragma once
- Include the component header as first include
- Do not defer behavior to comments
- Do not use comments or phrases such as: “In production” “TODO” “Example” “Placeholder” “Assume”
- Wrap implementations in namespace {self.namespace}
""".strip()

    def _batch_function_implementations(
        self,
        component: ComponentModel,
        batch_functions: List[FunctionModel],
    ) -> str:
        """Build implementation detail section for functions in this batch only."""
        lines = [
            f"=== FUNCTION IMPLEMENTATION DETAILS "
            f"(Batch: {len(batch_functions)} functions) ==="
        ]
        for func in batch_functions:
            lines.append(self._format_function_for_cpp(func))
        return "\n".join(lines)

    def _cpp_batch_output_instructions(
        self,
        component: ComponentModel,
        batch_functions: List[FunctionModel],
    ) -> str:
        """Output instructions specific to batch CPP generation."""
        func_list = "\n".join(f"  - {f.unit_name}" for f in batch_functions)
        return f"""
=== OUTPUT INSTRUCTIONS FOR BATCH CPP ===

Generate ONLY the implementations for these {len(batch_functions)} functions:
{func_list}

EXACT structure:
1. Doxygen @file block                                                     <- VERY FIRST, before any #include
2. #include "{component.component_name}/{component.component_name}.hpp"   <- first include
3. All other required standard library #include directives
4. namespace {self.namespace} opening
5. Implementation of ONLY the {len(batch_functions)} functions listed above

STRICT RULES:
- Do NOT declare any class or struct
- Do NOT add #pragma once
- Do NOT implement functions from other batches
- Do NOT add any explanation text outside the C++ code
- Each function MUST have full Doxygen block
- No stubs, no TODOs — complete implementations only

CRITICAL IMPLEMENTATION RULE:
- The pseudocode section is AUTHORITATIVE.
- Invoke pseudocode concretely in the generated code
- Every operation mentioned MUST be implemented.
- No operation may be omitted, replaced, or commented out.
- Variable names used in pseudocode MUST be reused exactly.
- Output ONLY valid standard C++ code
""".strip()

    def _header_task_intro(self, component: ComponentModel) -> str:
        return f"""
=== TASK: Generate Standard C++ Header File ===

You are generating a production-quality STANDARD C++ HEADER FILE (.hpp)
for the software component: {component.component_name}

Namespace  : {self.namespace}
File name  : {component.component_name}.hpp


Requirements:
- Standard C++14 / C++17 
- Complete Doxygen documentation on EVERY element (file, namespace, class,
  every method, every member variable, every constant, every enum value)
- #pragma once as the very first line
- Doxygen @file block immediately after #pragma once, BEFORE any #include
- All inter-component dependencies included using relative paths
- Only standard library headers — no ara:: or AUTOSAR headers
- Class declaration with ALL methods and private members
""".strip()

    def _cpp_task_intro(self, component: ComponentModel) -> str:
        return f"""
=== TASK: Generate Standard C++ Implementation File ===

You are generating a production-quality STANDARD C++ IMPLEMENTATION FILE (.cpp)
for the software component: {component.component_name}

Namespace  : {self.namespace}
File name  : {component.component_name}.cpp

The header file {component.component_name}.hpp was already generated in this conversation.
Your implementation MUST be 100% consistent with that header.

IMPORTANT: Generate STANDARD C++ ONLY.

Additional codegen tags (see **INTEGRATION_BUILD_RULES** above for full detail):
- **Header/cpp parity**: identical member and API spellings; no merged identifiers (e.g. `selectedHandlenull`).
- **Naming**: if the spec uses `_null` or trailing `_` on members, use it **consistently** in .hpp and .cpp (IB5–IB7).
- **Compile/link**: lambdas capture outer variables (IB10); one stub signature for CreateServiceProxy etc. (IB11–IB12);
  no duplicate `static` definitions (IB13); `probe::LogInfo` / `LogError` defined in `namespace probe` (IB9).
- **const**: no member writes in `const` methods unless `mutable` (IB14).
- Generation must produce compilable, linkable C++14/17; match header contract (const/noexcept/threading).
- No const_cast to mutate through const; choose mutex vs atomic consistently for shared state.
- Shared memory: validate offset+length; no unchecked memcpy.
- Use elapsed durations (now − start); base tick × period for 100 ms / 1000 ms boundaries.
- Keep core state in class instances; avoid hidden singletons unless specified.
- Prefer event-driven comm; explicit state machines where sequencing is complex.
- Platform I/O (logging, time, transport) via runtime abstraction when injected; stub bodies may print to cout/cerr for bring-up (IB17).
- Reserve buffers at init; avoid shrink_to_fit and O(n) erases in time-budgeted loops; deque/ring for cyclic data.
- Stubs: feature-gate or document; variant dictionary representation single-sourced across .hpp/.cpp.
- Startup I/O bounded with defined failure behavior; avoid heavy I/O in cyclic paths.
- Remove unused includes; prefer warning-clean builds; avoid heap in hot logging paths.


CRITICAL IMPLEMENTATION RULE:
- The pseudocode section is AUTHORITATIVE.
- Invoke pseudocode concretely in the generated code
- Every operation mentioned MUST be implemented.
- No operation may be omitted, replaced, or commented out.
- Variable names used in pseudocode MUST be reused exactly.
- Output ONLY valid standard C++ code
Requirements:
- Doxygen @file block at the very top BEFORE any #include directives
- Implement EVERY method declared in the header — no stubs, no TODOs
- Doxygen documentation on ALL internal helpers, structs, enums, constants
- Use pseudocode and detailed behavior descriptions below as implementation guide
- Standard C++14 / C++17 — no ara:: types, 
- Error handling: use return codes, std::optional, or exceptions —
- Logging: use std::cout / std::cerr or an injected logger —
- Include {component.component_name}.hpp as first include
""".strip()

    def _component_overview(self, component: ComponentModel) -> str:
        lines = [f"=== COMPONENT OVERVIEW: {component.component_name} ==="]
        lines.append(f"Total functions : {len(component.functions)}")

        if component.all_provided_interfaces:
            lines.append(f"Provided interfaces  : {', '.join(component.all_provided_interfaces)}")
        if component.all_required_interfaces:
            lines.append(f"Required interfaces  : {', '.join(component.all_required_interfaces)}")
        if component.all_dependent_components:
            lines.append(f"Dependent components : {', '.join(component.all_dependent_components)}")
        if component.all_service_interfaces:
            lines.append(f"Service interfaces   : {', '.join(component.all_service_interfaces)}")

        return "\n".join(lines)

    def _dependency_signatures(
        self,
        component: ComponentModel,
        global_interface_map: Dict[str, List[str]],
    ) -> str:
        deps = component.all_dependent_components
        if not deps:
            return ""

        lines = [
            "=== DEPENDENT COMPONENT SIGNATURES ===",
            "Use EXACTLY these signatures when making inter-component calls.",
            'Include each as: #include "../ComponentName/ComponentName.hpp"',
        ]

        for dep in deps:
            sigs = global_interface_map.get(dep, [])
            if sigs:
                lines.append(f"\n// {dep}")
                lines.append(f"class {dep} {{")
                lines.append("public:")
                for sig in sigs:
                    lines.append(f"    {sig}")
                lines.append("};")

        return "\n".join(lines)

    def _all_function_signatures(self, component: ComponentModel) -> str:
        lines = [f"=== ALL FUNCTIONS IN {component.component_name} ==="]
        for func in component.functions:
            lines.append(self._format_function_for_header(func))
        return "\n".join(lines)

    def _format_function_for_header(self, func: FunctionModel) -> str:
        lines = [
            f"\n--- Function: {func.unit_name} ---",
            f"Unit ID      : {func.unit_id}",
        ]

        if func.software_requirement_ids:
            lines.append(f"Requirements : {', '.join(func.software_requirement_ids)}")
        if func.description:
            lines.append(f"Description  : {func.description}")
        if func.unit_rationale:
            lines.append(f"Rationale    : {func.unit_rationale}")

        lines.append(self._format_signature_info(func))

        if func.timing_constraint:
            lines.append(f"Timing       : {func.timing_constraint}")
        if func.provided_interfaces:
            lines.append(f"Provides     : {', '.join(func.provided_interfaces)}")
        if func.required_interfaces:
            lines.append(f"Requires     : {', '.join(func.required_interfaces)}")
        if func.calling_class:
            lines.append(f"Called by    : {func.calling_class}")
        if func.call_condition:
            lines.append(f"Call condition: {func.call_condition}")


        lines.append(self._format_doxygen_hints(func))

        return "\n".join(lines)

    def _format_doxygen_hints(self, func: FunctionModel) -> str:
        """Generate Doxygen documentation hints specific to this function."""
        hints = [f"\nDoxygen hints for {func.unit_name}:"]

        if func.software_requirement_ids:
            hints.append(
                f"  @requirements {' '.join(func.software_requirement_ids)}"
            )
        if func.timing_constraint:
            hints.append(
                f"  @note Timing constraint: {func.timing_constraint}"
            )
        if func.calling_class and func.calling_class.lower() not in ("none", ""):
            hints.append(
                f"  @note Called by: {func.calling_class}"
            )
        # AFTER
        if func.error_handling:
            hints.append(f"  @retval false/error — see Error Handling section")
        if func.threading_model:
            hints.append(f"  @warning Threading: {func.threading_model[:120]}")
                # the LLM — they contain AUTOSAR-specific guidance that must be ignored.

        return "\n".join(hints)




    def _format_signature_info(self, func: FunctionModel) -> str:
        lines = []

        inputs  = func.input_signals
        types   = func.input_data_types
        params  = []
        for i, sig in enumerate(inputs):
            if sig and sig.lower() not in ("none", ""):
                dtype = types[i] if i < len(types) else "void"
                if dtype.lower() not in ("none", ""):
                    params.append(f"{dtype} {sig}")
        if params:
            lines.append(f"Parameters   : {', '.join(params)}")
        else:
            lines.append("Parameters   : none (void)")

        out_signals = [s for s in func.output_signals   if s and s.lower() != "none"]
        out_types   = [t for t in func.output_data_types if t and t.lower() != "none"]
        if out_signals and out_types:
            lines.append(f"Returns      : {out_types[0]} ({out_signals[0]})")
        elif out_types:
            lines.append(f"Return type  : {out_types[0]}")
        else:
            lines.append("Return type  : void")

        return "\n".join(lines)

    def _private_members_section(self, component: ComponentModel) -> str:
        members = component.get_all_private_members()
        if not members:
            return ""

        lines = [
            "=== PRIVATE MEMBER VARIABLES ===",
            "Declare these as private member variables in the class.",
            "Every member MUST have an ///< @brief inline Doxygen comment.",
            "Infer the correct standard C++ type from context and usage.",
            "Do NOT use any ara:: or AUTOSAR types — use std:: equivalents only.",
        ]
        for m in members:
            lines.append(f"  - {m}")
        return "\n".join(lines)

    def _interfaces_section(self, component: ComponentModel) -> str:
        """
        Renders provided/required/service interface information for the component.
        AUTOSAR service concepts (OfferService, FindService, etc.) are stripped —
        only the logical interface names are forwarded to the LLM.
        """
        services = component.all_service_interfaces
        provided = component.all_provided_interfaces
        required = component.all_required_interfaces

        if not any([services, provided, required]):
            return ""

        lines = [
            "=== COMPONENT INTERFACES ===",
            "NOTE: These are logical interface names only.",
            "Implement them as plain C++ class methods or callback registrations.",
            
        ]
        if provided:
            lines.append("Provided (implement as public methods or callback registrations):")
            for svc in provided:
                lines.append(f"  - {svc}")
        if required:
            lines.append("Required (call via constructor-injected dependency or direct include):")
            for svc in required:
                lines.append(f"  - {svc}")
        if services:
            lines.append("Connected interfaces (treat as plain inter-component dependencies):")
            for svc in services:
                lines.append(f"  - {svc}")
        return "\n".join(lines)

    def _header_output_instructions(self, component: ComponentModel) -> str:
        return f"""
=== OUTPUT INSTRUCTIONS FOR HEADER ===

Generate ONLY the complete {component.component_name}.hpp file.

EXACT structure order:
1. #pragma once                          <- very first line
2. Doxygen @file block                   <- BEFORE any #include
3. All #include directives (standard library only — NO ara:: headers)
4. @defgroup or @ingroup declaration
5. namespace {self.namespace} opening
6. @namespace Doxygen block
7. Class Doxygen block (@class, @brief, @details, @ingroup, @note, @warning, @invariant, @see)
8. Class declaration with:
   - All public methods with FULL Doxygen (@brief, @details, @param[in/out], @return, @retval, @pre, @post, @throws, @note, @warning, @requirements, @rationale)
   - All private member variables with ///< @brief inline comments
   - All private methods with full Doxygen
   - All constexpr/const with @brief @details
9. namespace closing brace

STRICT RULES:
- Do NOT write "@return void" — omit @return entirely for void functions
- Do NOT use bare @param — always @param[in], @param[out], or @param[in,out]
- Do NOT place @file block after any #include
- Do NOT leave any element undocumented
- Do NOT include any ara:: headers (ara/core, ara/log, ara/com, ara/exec, etc.)
- Do NOT use any ara:: types anywhere in the code
- Do NOT add any explanation text outside the C++ code
- Output ONLY valid standard C++ code
""".strip()

    # ------------------------------------------------------------------ #
    # CPP prompt sections
    # ------------------------------------------------------------------ #

    def _all_function_implementations(self, component: ComponentModel) -> str:
        lines = [
            f"=== FUNCTION IMPLEMENTATION DETAILS FOR {component.component_name} ==="
        ]
        for func in component.functions:
            lines.append(self._format_function_for_cpp(func))
        return "\n".join(lines)

    def _format_function_for_cpp(self, func: FunctionModel) -> str:
        lines = [
            f"\n{'='*60}",
            f"IMPLEMENT: {func.unit_name}",
            f"Unit ID  : {func.unit_id}",
            f"{'='*60}",
        ]

        lines.append(self._format_signature_info(func))

        if func.timing_constraint:
            lines.append(f"Timing     : {func.timing_constraint}")
        if func.calling_class:
            lines.append(f"Called by  : {func.calling_class} | Condition: {func.call_condition}")
        if func.function_calls:
            lines.append(f"Calls      : {', '.join(func.function_calls)}")
        if func.dependent_components:
            lines.append(f"Inter-comp : {', '.join(func.dependent_components)}")

        if func.detailed_behavior:
            lines.append("\nDetailed Behavior:")
            lines.append(func.detailed_behavior)

        if func.pseudocode:
            lines.append(
                "\nPseudocode (implement all logic below; if a function appears, invoke it concretely; "
                "translate to standard C++ — ignore any ara:: references):"
            )
            lines.append(func.pseudocode)

        if func.error_handling:
            lines.append("\nError Handling (use std::optional / return codes / exceptions — NOT ara::core::Result):")
            lines.append(func.error_handling)

        if func.threading_model:
            lines.append("\nThreading Model (use std::mutex, std::thread, std::atomic):")
            lines.append(func.threading_model)

        if func.external_interfaces_md:
            lines.append("\nExternal Interfaces:")
            lines.append(func.external_interfaces_md)
        if func.internal_interfaces_md:
            lines.append("\nInternal Interfaces:")
            lines.append(func.internal_interfaces_md)


        # adaptive_platform_notes intentionally suppressed — AUTOSAR-specific,
        # must not be forwarded to the LLM.

        if func.dependencies:
            lines.append("\nDependencies:")
            lines.append(func.dependencies)

        if func.private_members:
            lines.append(f"\nPrivate members used: {', '.join(func.private_members)}")

        if func.software_requirement_ids:
            lines.append(
                f"\nRequirements traceability: {', '.join(func.software_requirement_ids)}"
            )

        lines.append(self._format_doxygen_hints(func))

        return "\n".join(lines)

    def _state_machine_image_notice(self, component: ComponentModel) -> str:
        """Add a note in the prompt if state machine images are being sent."""
        funcs_with_images = [
            f.unit_name for f in component.functions if f.state_machine_images
        ]
        if not funcs_with_images:
            return ""
        return (
            "=== STATE MACHINE DIAGRAMS ===\n"
            "The following function(s) have State Machine diagrams attached as images:\n"
            + "\n".join(f"  - {n}" for n in funcs_with_images)
            + "\n\nFor each function with a state machine image:\n"
            "- Use the diagram to identify all states, transitions, and conditions\n"
            "- Implement the state machine using an enum class for states\n"
            "- Use a switch statement or state handler pattern\n"
            "- Ensure all transitions shown in the diagram are handled in code\n"
            "- Use standard C++ only — no AUTOSAR state management APIs"
        )

    def _cpp_output_instructions(self, component: ComponentModel) -> str:
        return f"""
=== OUTPUT INSTRUCTIONS FOR CPP ===

Generate ONLY the complete {component.component_name}.cpp file.

EXACT structure order:
1. Doxygen @file block                                      <- VERY FIRST — before any #include
2. #include "{component.component_name}/{component.component_name}.hpp"   <- first include (match -I.)
3. All other required standard library #include directives  <- NO ara:: headers
4. namespace {self.namespace} opening
5. Implementation of EVERY method from the header

FOR EACH METHOD IMPLEMENTATION:
- Add full Doxygen block (@brief, @details, @param[in/out], @retval, @pre, @post,
  @throws, @note, @warning, @requirements, @rationale)
- Translate pseudocode into valid standard C++14 / C++17
- Error handling: use return codes, std::optional<T>, or throw exceptions — NOT ara::core::Result
- Logging: use std::cout / std::cerr — NOT ara::log
- Inter-component calls: use direct method calls via injected references — NOT ara::com
- Every internal helper, struct, enum, constant MUST also be Doxygen documented

STRICT RULES:
- No stubs, no TODOs, no placeholder implementations
- Do NOT write "@return void" — omit @return for void functions
- Do NOT use bare @param — always use [in]/[out]/[in,out]
- Do NOT place @file block after any #include
- Do NOT include any ara:: headers (ara/core, ara/log, ara/com, ara/exec, etc.)
- Do NOT use any ara:: types, namespaces, 
- Wrap everything in namespace {self.namespace}
- Do NOT add any explanation text outside the C++ code
- Output ONLY valid standard C++ code
""".strip()
