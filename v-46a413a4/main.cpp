/**
 * @file   main.cpp
 * @brief  Application entry point — portable C++ with SimRuntime.
 *
 * This file provides the main() entry point for the ProbeApp application.
 * It uses the portable SimRuntime abstraction instead of ara:: APIs.
 * No business logic resides here — main() is purely a launcher.
 */

#include <cstdint>
#include <exception>
#include <string>

#include "runtime/SimRuntime.hpp"
#include "ProbeApp/ProbeApp.hpp"

/**
 * @brief  Application entry point.
 * @return 0 on successful execution, 1 on failure.
 */
int main()
{
    /* 1. Create portable runtime simulation (replaces ara::exec + ara::log + ara::com) */
    runtime::SimRuntime simRuntime;

    /* 2. Report execution state (replaces ara::exec::ExecutionClient::ReportExecutionState) */
    simRuntime.reportExecutionState("kRunning");

    /* 3. Log application start (replaces ara::log) */
    simRuntime.logInfo("[APP] Application starting...");

    /* 4. Instantiate orchestrator and call entry function inside try/catch */
    try
    {
        probe::ProbeApp app(simRuntime);

        /* 5. Call entry function — orchestrator manages all sub-components internally */
        static_cast<void>(app.ProbeApp());
    }
    catch (const std::exception& e)
    {
        simRuntime.logError(
            std::string("[APP] Exception: ") + e.what());
        simRuntime.reportExecutionState("kTerminating");
        return 1;
    }
    catch (...)
    {
        simRuntime.logError(
            "[APP] Unknown exception — application terminated.");
        simRuntime.reportExecutionState("kTerminating");
        return 1;
    }

    /* 6. Shutdown */
    simRuntime.logInfo("[APP] Application shutting down.");
    simRuntime.reportExecutionState("kTerminating");

    return 0;
}