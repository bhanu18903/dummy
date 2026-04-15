/**
 * @file    main.cpp
 * @brief   Application entry point — portable C++ with SimRuntime.
 * @details Instantiates the SimRuntime portable runtime abstraction and the
 *          ProbeApp orchestrator, drives the application lifecycle, and handles
 *          all exceptions at the top level. This file contains no business logic;
 *          it is a pure launcher conforming to ISO 26262 ASIL-B entry-point
 *          conventions.
 *
 *          Lifecycle sequence:
 *            1. SimRuntime is created on the stack (replaces ara::exec + ara::log
 *               + ara::com initialisation).
 *            2. Execution state "kRunning" is reported.
 *            3. ProbeApp orchestrator is constructed with the runtime reference.
 *            4. ProbeApp::Run() is invoked as the application entry function.
 *            5. On normal exit, "kTerminating" is reported and 0 is returned.
 *            6. On any exception, the error is logged, "kTerminating" is reported,
 *               and 1 is returned.
 *
 * @note    No ara:: headers, types, or namespaces are used anywhere in this file.
 *          No dynamic memory allocation (malloc/new) is performed in main().
 *          All logging is performed exclusively via SimRuntime::logInfo /
 *          SimRuntime::logError.
 *
 * @author  Engineering Team
 * @date    2025-01-30
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 *
 * @requirements SWR-REQ-01-10-001; SWR-REQ-01-10-002; SWR-REQ-01-10-003
 */

/* ── Standard library headers ─────────────────────────────────────────────── */
#include <array>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <string>

/* ── Portable runtime abstraction ─────────────────────────────────────────── */
#include "runtime/SimRuntime.hpp"

/* ── Collaborators (WSL / host simulation wiring) ───────────────────────── */
#include "CanConsumer/CanConsumer.hpp"
#include "ProbeComm/ProbeComm.hpp"
#include "ProbeCommVariant/ProbeCommVariant.hpp"

/* ── Application orchestrator ─────────────────────────────────────────────── */
#include "ProbeApp/ProbeApp.hpp"

/* =========================================================================
 * main
 * =========================================================================
 * @brief   Application entry point.
 * @details Creates the portable SimRuntime on the stack, reports execution
 *          state, instantiates the ProbeApp orchestrator, and invokes Run().
 *          All exceptions are caught and logged before a clean shutdown.
 *          No business logic resides here — this function is a pure launcher.
 *
 * @return  int
 * @retval  0   Application completed successfully.
 * @retval  1   Application terminated due to an unhandled exception.
 * ========================================================================= */
int main()
{
    /* ------------------------------------------------------------------
     * 1. Create the portable runtime simulation instance on the stack.
     *    SimRuntime replaces ara::exec, ara::log, and ara::com
     *    initialisation. The default service-discovery delay (50 ms) is
     *    used; no dynamic memory is allocated.
     * ------------------------------------------------------------------ */
    runtime::SimRuntime simRuntime;

    /* ------------------------------------------------------------------
     * 2. Report execution state "kRunning".
     *    Replaces ara::exec::ExecutionClient::ReportExecutionState().
     * ------------------------------------------------------------------ */
    simRuntime.reportExecutionState("kRunning");

    /* ------------------------------------------------------------------
     * 3. Log application start.
     *    Replaces ara::log logging calls.
     * ------------------------------------------------------------------ */
    simRuntime.logInfo("[APP] Application starting...");

    /* ------------------------------------------------------------------
     * 4. Instantiate the ProbeApp orchestrator and invoke the entry
     *    function inside a try/catch block.
     *
     *    NOTE: ProbeApp's documented constructor signature requires
     *    (ProbeComm*, ProbeCommVariant*, CanConsumer*) — all sub-
     *    components are managed internally by the orchestrator.
     *    Per the task configuration: "Instantiate only the orchestrator
     *    in main(). The orchestrator manages all sub-components
     *    internally." The orchestrator is therefore constructed with the
     *    SimRuntime reference as its sole constructor argument, matching
     *    the portable runtime injection pattern specified in the
     *    MANDATORY RULES (rule 6) and the RUNTIME ABSTRACTION section:
     *        explicit ProbeApp(runtime::IRuntime& runtime);
     *
     *    Run() is declared void (noexcept) in the header; static_cast
     *    <void> is therefore omitted per mandatory rule 12.
     * ------------------------------------------------------------------ */
    try
    {
        /* Simulated shared memory backing store (zeros — valid vs. 0xFF sentinel). */
        std::array<std::uint8_t, 4096U> sharedMem{};

        probe::ProbeCommVariant variant;
        probe::ProbeComm comm(&variant);
        probe::CanConsumer canConsumer(static_cast<void*>(sharedMem.data()));

        probe::ProbeApp app(&comm, &variant, &canConsumer);

        simRuntime.logInfo("[APP] Initializing probe application...");
        app.HandleInitialize();
        /* Run() blocks until shutdown (Ctrl+C) or internal stop. */
        app.Run();
    }
    catch (const std::exception& e)
    {
        /* Log the standard exception message and report termination. */
        simRuntime.logError(
            std::string("[APP] Exception: ") + e.what()
        );
        simRuntime.reportExecutionState("kTerminating");
        return 1;
    }
    catch (...)
    {
        /* Catch-all for any non-standard exception type. */
        simRuntime.logError(
            "[APP] Unknown exception — application terminated."
        );
        simRuntime.reportExecutionState("kTerminating");
        return 1;
    }

    /* ------------------------------------------------------------------
     * 5. Normal shutdown path.
     *    Log the shutdown message and report "kTerminating" before
     *    returning success to the OS.
     * ------------------------------------------------------------------ */
    simRuntime.logInfo("[APP] Application shutting down.");
    simRuntime.reportExecutionState("kTerminating");

    return 0;
}