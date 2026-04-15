#pragma once

/**
 * @file    ProbeApp.hpp
 * @brief   Declares the ProbeApp class responsible for probe data collection and transmission.
 * @details ProbeApp orchestrates cyclic CAN signal sampling, log data set assembly,
 *          event-triggered data collection, image acquisition, and transmission of
 *          steady-state and event data to DAQ and GEDR targets via ProbeComm.
 *          It manages ring buffers for 100 ms and 1000 ms CAN data, per-second log
 *          data sets, and event/image buffers, enforcing retention limits and
 *          safe shutdown semantics in compliance with ISO 26262 ASIL-B requirements.
 * @author  Engineering Team
 * @date    2025-01-30
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include "../ProbeComm/ProbeComm.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

/**
 * @defgroup ProbeApplication Probe Application Components
 * @brief Components responsible for probe data collection, buffering, and transmission.
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

class CanConsumer;

/**
 * @class   ProbeApp
 * @brief   Top-level application class for probe data collection and transmission.
 * @details ProbeApp is the central orchestrator of the probe subsystem. It reads CAN
 *          and internal signals from shared memory at 100 ms and 1000 ms sampling rates,
 *          assembles per-second log data sets, handles event-triggered data collection
 *          including image acquisition, and transmits steady-state and event data to
 *          DAQ and GEDR targets through the ProbeComm interface.
 *
 *          Lifecycle:
 *            1. Constructed via ProbeApp(ProbeComm*, ProbeCommVariant*, CanConsumer*).
 *            2. HandleInitialize() called once after ECU boot to start SOME/IP services.
 *            3. Run() called cyclically (10 ms task) to drive all data collection and
 *               transmission logic.
 *            4. HandleTerminate() or HandleShutdown() called on ECU lifecycle events.
 *
 *          Thread-safety:
 *            All shared buffers are protected by dedicated std::mutex members.
 *            Atomic flags are used for lifecycle state variables.
 *
 * @ingroup ProbeApplication
 * @note    Complies with ISO 26262 ASIL-B, MISRA C++:2008, and CERT C++ guidelines.
 *          No dynamic memory allocation (new/delete/malloc) is used after construction.
 *          All heap-managed objects use std::unique_ptr or std::shared_ptr (RAII).
 * @warning Do not call Run() before HandleInitialize() completes successfully.
 *          Do not share a ProbeApp instance across threads without external synchronisation
 *          of the lifecycle methods (HandleInitialize, Run, HandleTerminate, HandleShutdown).
 * @invariant probeComm_ is never nullptr after successful construction.
 * @invariant bufferMutex100ms_, bufferMutex1000ms_, and logBufferMutex_ are always
 *            released before any public method returns.
 * @see     ProbeComm
 */
class ProbeApp {
public:

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    /**
     * @brief   Constructs ProbeApp with references to communication, variant, and CAN consumer.
     * @details Initialises all internal state, ring buffers, and variant-specific behaviour.
     *          Stores non-owning observer pointers to the provided collaborators; their
     *          lifetimes must exceed that of this ProbeApp instance.
     *          All member variables are initialised to safe default values in the
     *          constructor initialiser list.
     * @param[in] comm      Non-owning pointer to the ProbeComm communication interface.
     *                      Must not be nullptr. Lifetime must exceed this object.
     * @param[in] variant   Non-owning pointer to the ProbeCommVariant strategy object.
     *                      Must not be nullptr. Lifetime must exceed this object.
     * @param[in] canCons   Non-owning pointer to the CanConsumer shared-memory reader.
     *                      Must not be nullptr. Lifetime must exceed this object.
     * @pre     comm != nullptr && variant != nullptr && canCons != nullptr.
     * @post    All buffers are empty; isRunning_ == false; isDaqCommEstablished_ == false.
     * @throws  None — constructor is noexcept; invalid pointers are caught by precondition.
     * @note    Called by: Main at startup.
     * @warning Passing nullptr for any parameter results in undefined behaviour.
     * @requirements SWR-REQ-01-01-002;SWR-REQ-01-01-003;SWR-REQ-01-02-001
     * @rationale Dependency injection via constructor enables unit-testing with mock
     *            collaborators without modifying production code.
     * @see     HandleInitialize
     */
    explicit ProbeApp(ProbeComm* comm,
                      ProbeCommVariant* variant,
                      CanConsumer* canCons) noexcept;

    /**
     * @brief   Destroys the ProbeApp instance and releases all owned resources.
     * @details Ensures all RAII-managed resources (mutexes, buffers) are released
     *          cleanly. Does not perform network communication; call HandleShutdown()
     *          before destruction if graceful teardown is required.
     * @pre     HandleTerminate() or HandleShutdown() has been called.
     * @post    All resources owned by this instance are released.
     * @throws  None.
     */
    ~ProbeApp() noexcept;

    /// @brief Deleted copy constructor — ProbeApp is non-copyable.
    ProbeApp(const ProbeApp&) = delete;

    /// @brief Deleted copy-assignment operator — ProbeApp is non-copyable.
    ProbeApp& operator=(const ProbeApp&) = delete;

    /// @brief Deleted move constructor — ProbeApp is non-movable.
    ProbeApp(ProbeApp&&) = delete;

    /// @brief Deleted move-assignment operator — ProbeApp is non-movable.
    ProbeApp& operator=(ProbeApp&&) = delete;

    // =========================================================================
    // Lifecycle Methods
    // =========================================================================

    /**
     * @brief   Starts ProbeApp operation after ADAS ECU startup and initiates SOME/IP
     *          communication with DAQ.
     * @details Triggers service discovery for all required SOME/IP services via ProbeComm,
     *          offers the ProbeEventStt and ProbeTrigger services, and resets the startup
     *          timer. Must be called exactly once after construction and before Run().
     * @pre     ProbeApp has been successfully constructed.
     * @post    SOME/IP service discovery is in progress; startup timer is reset.
     * @throws  None.
     * @note    Called by: Main after ECU boot sequence completes.
     * @warning Calling this method more than once results in undefined behaviour.
     * @requirements SWR-REQ-01-10-001;SWR-REQ-01-10-002;SWR-REQ-01-03-001;
     *               SWR-REQ-03-16-001;SWR-REQ-03-16-002
     * @see     Run, HandleTerminate
     */
    void HandleInitialize() noexcept;

    /**
     * @brief   Runs the cyclic data collection and transmission processing loop.
     * @details Executes one iteration of the main processing cycle. Intended to be
     *          called from a 10 ms periodic task. Each call performs:
     *            - Shared memory signal collection (CollectSharedMemReadSignals),
     *            - CAN data storage at 100 ms and 1000 ms rates,
     *            - Event signal data storage,
     *            - Per-second log data set assembly,
     *            - Oldest log data set deletion when retention limit is exceeded,
     *            - Regular data transmission to DAQ,
     *            - Transmitted steady-state data deletion.
     *          The method is a no-op if isRunning_ is false or shutdownRequested_ is true.
     * @pre     HandleInitialize() has been called successfully.
     * @post    Buffers and transmission state are updated for this cycle.
     * @throws  None.
     * @note    Called by: Main cyclic 10 ms task.
     * @warning Must not be called concurrently from multiple threads.
     * @requirements SWR-REQ-01-001;SWR-REQ-01-002;SWR-REQ-01-005;SWR-REQ-01-06-001;
     *               SWR-REQ-01-10-003;SWR-REQ-03-16-003
     * @see     HandleInitialize, HandleTerminate
     */
    void Run() noexcept;

    /**
     * @brief   Stops all data collection and transmission processing on termination.
     * @details Sets isRunning_ to false so that subsequent Run() calls are no-ops.
     *          Does not clear RAM buffers; retained data may be used in the next
     *          drive cycle if applicable.
     * @pre     HandleInitialize() has been called.
     * @post    isRunning_ == false; no further data collection or transmission occurs.
     * @throws  None.
     * @note    Called by: Main on ECU termination event.
     * @requirements SWR-REQ-01-09-001;SWR-REQ-03-13-001
     * @see     HandleShutdown
     */
    void HandleTerminate() noexcept;

    /**
     * @brief   Stops all processing and deletes all temporary data including
     *          mid-transmission data on ADAS ECU shutdown.
     * @details Sets shutdownRequested_ to true, stops all collection and transmission,
     *          then calls ClearAllTemporaryData() to purge every RAM buffer including
     *          data currently being transmitted. Ensures a clean state before power-off.
     * @pre     HandleInitialize() has been called.
     * @post    isRunning_ == false; shutdownRequested_ == true; all buffers are empty.
     * @throws  None.
     * @note    Called by: Main on ECU shutdown event.
     * @warning This method discards all in-flight transmission data. Ensure DAQ has
     *          acknowledged any critical data before invoking shutdown.
     * @requirements SWR-REQ-01-09-001;SWR-REQ-01-09-002;SWR-REQ-03-13-001;
     *               SWR-REQ-03-13-002;SWR-REQ-03-13-003;SWR-REQ-01-08-003
     * @see     HandleTerminate, ClearAllTemporaryData
     */
    void HandleShutdown() noexcept;

    // =========================================================================
    // Signal Collection Methods
    // =========================================================================

    /**
     * @brief   Reads CAN and internal signals from shared memory at configured
     *          sampling rates.
     * @details Invokes the CanConsumer interface to read all configured signal groups
     *          from shared memory. Dispatches to StoreCanData100ms() and
     *          StoreCanData1000ms() based on the current msCounter_ value.
     *          Also triggers StoreEventSignalData() for event-driven signals.
     * @pre     isRunning_ == true; CanConsumer pointer is valid.
     * @post    canBuffer100ms_ and/or canBuffer1000ms_ are updated with fresh samples.
     * @throws  None.
     * @note    Called by: ProbeApp::Run() every 10 ms cycle.
     * @requirements SWR-REQ-01-01-001;SWR-REQ-01-001;SWR-REQ-01-002;SWR-REQ-01-003
     * @see     StoreCanData100ms, StoreCanData1000ms, StoreEventSignalData
     */
    void CollectSharedMemReadSignals() noexcept;

    /**
     * @brief   Reads CAN signals at 100 ms rate and stores them into the 100 ms ring
     *          buffer with timestamp.
     * @details Acquires bufferMutex100ms_, reads the current CAN signal snapshot from
     *          canConsumer_, appends a timestamped entry to canBuffer100ms_, and
     *          releases the mutex. Overwrites the oldest entry when the buffer is full.
     * @pre     canConsumer_ != nullptr; bufferMutex100ms_ is not held by the caller.
     * @post    canBuffer100ms_ contains one additional timestamped entry (or oldest
     *          entry overwritten if buffer was full).
     * @throws  None.
     * @note    Called by: ProbeApp::CollectSharedMemReadSignals() every 100 ms.
     * @requirements SWR-REQ-01-001;SWR-REQ-01-01-001;SWR-REQ-01-01-002;SWR-REQ-01-003
     * @see     StoreCanData1000ms, CollectSharedMemReadSignals
     */
    void StoreCanData100ms() noexcept;

    /**
     * @brief   Reads CAN signals at 1000 ms rate and stores them into the 1000 ms ring
     *          buffer with timestamp.
     * @details Acquires bufferMutex1000ms_, reads the current CAN signal snapshot from
     *          canConsumer_, appends a timestamped entry to canBuffer1000ms_, and
     *          releases the mutex. Overwrites the oldest entry when the buffer is full.
     * @pre     canConsumer_ != nullptr; bufferMutex1000ms_ is not held by the caller.
     * @post    canBuffer1000ms_ contains one additional timestamped entry (or oldest
     *          entry overwritten if buffer was full).
     * @throws  None.
     * @note    Called by: ProbeApp::CollectSharedMemReadSignals() every 1000 ms.
     * @requirements SWR-REQ-01-002;SWR-REQ-01-01-001;SWR-REQ-01-01-002;SWR-REQ-01-003
     * @see     StoreCanData100ms, CollectSharedMemReadSignals
     */
    void StoreCanData1000ms() noexcept;

    /**
     * @brief   Collects event-based signals with timestamps, signal IDs, and data size
     *          information.
     * @details Polls the event signal sources for new data. For each new event signal,
     *          records the signal ID, timestamp (ms resolution), data size, and raw
     *          payload into the event sending buffer. Thread-safe via logBufferMutex_.
     * @pre     isRunning_ == true.
     * @post    eventSendingBuffer_ contains any newly arrived event signal entries.
     * @throws  None.
     * @note    Called by: ProbeApp::CollectSharedMemReadSignals() every 10 ms cycle.
     * @requirements SWR-REQ-03-01-002;SWR-REQ-03-01-004;SWR-REQ-03-01-005;
     *               SWR-REQ-03-01-006;SWR-REQ-03-01-007
     * @see     BuildLogDataSetPerSecond
     */
    void StoreEventSignalData() noexcept;

    // =========================================================================
    // Log Data Set Management
    // =========================================================================

    /**
     * @brief   Assembles per-second log data sets per the collection data retention
     *          layout specification.
     * @details Every 1000 ms, combines the 100 ms CAN buffer entries, 1000 ms CAN
     *          buffer entries, and event signal data collected during the past second
     *          into a single log data set entry in logDataSetBuffer_. Acquires
     *          logBufferMutex_ for the duration of the assembly.
     * @pre     isRunning_ == true; canBuffer100ms_ and canBuffer1000ms_ contain
     *          valid data for the current second.
     * @post    logDataSetBuffer_ contains one additional per-second log data set.
     * @throws  None.
     * @note    Called by: ProbeApp::Run() once per second.
     * @requirements SWR-REQ-03-01-008;SWR-REQ-03-001
     * @see     DeleteOldestLogDataSet, BuildLogDataSetPerSecond
     */
    void BuildLogDataSetPerSecond() noexcept;

    /**
     * @brief   Removes the oldest log data set when the retention count is exceeded.
     * @details Acquires logBufferMutex_ and removes the front element of
     *          logDataSetBuffer_ if its size exceeds pastDataRetentionCount_.
     *          Ensures the rolling buffer does not grow beyond the configured
     *          retention window.
     * @pre     logBufferMutex_ is not held by the caller.
     * @post    logDataSetBuffer_.size() <= pastDataRetentionCount_.
     * @throws  None.
     * @note    Called by: ProbeApp::Run() after BuildLogDataSetPerSecond().
     * @requirements SWR-REQ-03-01-009;SWR-REQ-03-001
     * @see     BuildLogDataSetPerSecond
     */
    void DeleteOldestLogDataSet() noexcept;

    /**
     * @brief   Sends zero-filled data blocks for the pre-trigger time window where log
     *          data is not yet available after IG-ON.
     * @details When a trigger event occurs before the pre-trigger retention window is
     *          fully populated (i.e., shortly after IG-ON), this method generates
     *          zero-filled log data set blocks to cover the missing time slots from
     *          startTime up to the first available real log data set.
     * @param[in] startTime  Relative start time in milliseconds from IG-ON for the
     *                       pre-trigger window. Valid range: [INT32_MIN, 0].
     *                       Negative values indicate time before the trigger.
     * @pre     startTime <= 0; isRunning_ == true.
     * @post    Zero-filled data blocks covering [startTime, first_real_data) are
     *          appended to the event sending buffer.
     * @throws  None.
     * @note    Called by: ProbeApp on event trigger detection.
     * @requirements SWR-REQ-03-002;SWR-REQ-03-01-011;SWR-REQ-03-07-007
     * @rationale After IG-ON, the rolling log buffer is not yet full. Zero-fill ensures
     *            the DAQ receives a complete and consistently-sized data set regardless
     *            of when the trigger occurs relative to IG-ON.
     * @see     BuildLogDataSetPerSecond
     */
    void FillZeroDataForMissingPreTrigger(int32_t startTime) noexcept;

    // =========================================================================
    // Buffer Management
    // =========================================================================

    /**
     * @brief   Deletes steady-state data from RAM after successful transmission to DAQ.
     * @details Clears steadyStateDataBuffer100ms_ and steadyStateDataBuffer1000ms_
     *          after confirming that the data has been acknowledged by DAQ. Acquires
     *          bufferMutex100ms_ and bufferMutex1000ms_ in a consistent order to
     *          prevent deadlock.
     * @pre     Steady-state data has been successfully transmitted and acknowledged.
     * @post    steadyStateDataBuffer100ms_ and steadyStateDataBuffer1000ms_ are empty.
     * @throws  None.
     * @note    Called by: ProbeApp after successful DAQ transmission confirmation.
     * @requirements SWR-REQ-01-006;SWR-REQ-01-07-001
     * @see     DeleteTransmittedSteadyStateData, ClearAllTemporaryData
     */
    void ClearSteadyStateBuffer() noexcept;

    /**
     * @brief   Clears all RAM buffers including regular, event, image, and
     *          mid-transmission data.
     * @details Acquires all buffer mutexes in a defined order and clears:
     *          canBuffer100ms_, canBuffer1000ms_, logDataSetBuffer_,
     *          steadyStateDataBuffer100ms_, steadyStateDataBuffer1000ms_,
     *          eventSendingBuffer_, and imageDataBuffer_. Used during shutdown
     *          to ensure no stale data remains in RAM.
     * @pre     shutdownRequested_ == true or isRunning_ == false.
     * @post    All data buffers are empty.
     * @throws  None.
     * @note    Called by: ProbeApp::HandleShutdown().
     * @warning This operation is irreversible. All in-flight transmission data is lost.
     * @requirements SWR-REQ-01-09-002;SWR-REQ-03-13-002
     * @see     HandleShutdown, ClearSteadyStateBuffer
     */
    void ClearAllTemporaryData() noexcept;

    // =========================================================================
    // Startup and Safety Guards
    // =========================================================================

    /**
     * @brief   Verifies that data collection can start after the 10-second delay
     *          from IG-ON.
     * @details Compares the elapsed time since IG-ON (tracked via startupTimeSec_)
     *          against the mandatory 10-second startup delay. Returns true only when
     *          the delay has elapsed and collection has not yet been flagged as started.
     * @pre     HandleInitialize() has been called; startupTimeSec_ is being incremented.
     * @post    If true is returned, collectionStarted_ is set to true.
     * @return  bool indicating whether data collection may begin.
     * @retval  true   The 10-second startup delay has elapsed and collection may start.
     * @retval  false  The startup delay has not yet elapsed or collection already started.
     * @throws  None.
     * @note    Called by: ProbeApp::Run() during startup phase.
     * @requirements SWR-REQ-01-10-003;SWR-REQ-03-01-001;SWR-REQ-03-16-003
     * @rationale A mandatory 10-second delay after IG-ON ensures ECU subsystems and
     *            SOME/IP services are fully initialised before data collection begins,
     *            preventing capture of transient startup artefacts.
     * @see     HandleInitialize, Run
     */
    bool CheckCollectionStartDelay() noexcept;

    /**
     * @brief   Halts application if RAM cleanup fails to prevent inconsistent state.
     * @details If a RAM buffer deletion operation fails (detected by
     *          HandleRamDeletionFailure()), this method sets isRunning_ to false and
     *          shutdownRequested_ to true, preventing further data collection or
     *          transmission in an inconsistent state.
     * @pre     A RAM deletion failure has been detected.
     * @post    isRunning_ == false; shutdownRequested_ == true.
     * @throws  None.
     * @note    Called by: ProbeApp on RAM deletion failure detection.
     * @warning This is a safety-critical stop. Once called, the application must be
     *          restarted to resume normal operation.
     * @requirements SWR-REQ-01-08-003;SWR-REQ-03-13-003
     * @see     HandleRamDeletionFailure
     */
    void EnforceStopOnRamCleanupFailure() noexcept;

    // =========================================================================
    // Data Transmission Methods
    // =========================================================================

    /**
     * @brief   Transmits steady-state sampled data to DAQ via SOME/IP UDP
     *          fire-and-forget at 100 ms and 1000 ms cycles.
     * @details Checks isDaqCommEstablished_ before attempting transmission.
     *          Calls ProbeComm::SendContinualShortDataCyclic100ms() with the
     *          100 ms buffer payload and ProbeComm::SendContinualLongDataCyclic1000ms()
     *          with the 1000 ms buffer payload at their respective cycle rates.
     *          Acquires the appropriate buffer mutex for each send operation.
     * @pre     isDaqCommEstablished_ == true; isRunning_ == true.
     * @post    Steady-state data payloads have been submitted to ProbeComm for
     *          transmission; steadyStateDataBuffer100ms_ and/or
     *          steadyStateDataBuffer1000ms_ are marked as transmitted.
     * @throws  None.
     * @note    Called by: ProbeApp::Run() at 100 ms and 1000 ms cycle points.
     * @requirements SWR-REQ-01-005;SWR-REQ-01-06-001;SWR-REQ-01-06-002;
     *               SWR-REQ-01-08-001;SWR-REQ-01-08-002
     * @see     DeleteTransmittedSteadyStateData, ProbeComm::SendContinualShortDataCyclic100ms
     */
    void SendRegularData() noexcept;

    /**
     * @brief   Removes transmitted steady-state data from RAM buffer after successful
     *          or failed transmission.
     * @details After SendRegularData() completes (regardless of DAQ acknowledgement
     *          outcome), this method clears the transmitted entries from
     *          steadyStateDataBuffer100ms_ and steadyStateDataBuffer1000ms_.
     *          On transmission failure, delegates to HandleRamDeletionFailure() if
     *          the buffer clear itself fails.
     * @pre     SendRegularData() has been called in the current cycle.
     * @post    Transmitted entries are removed from steady-state buffers.
     * @throws  None.
     * @note    Called by: ProbeApp::Run() after SendRegularData().
     * @requirements SWR-REQ-01-006;SWR-REQ-01-07-001;SWR-REQ-01-08-002
     * @see     SendRegularData, HandleRamDeletionFailure, ClearSteadyStateBuffer
     */
    void DeleteTransmittedSteadyStateData() noexcept;

    /**
     * @brief   Initiates controlled stop if RAM buffer deletion fails after
     *          communication error.
     * @details Called when a buffer clear operation in DeleteTransmittedSteadyStateData()
     *          or ClearAllTemporaryData() cannot complete successfully. Logs the failure
     *          condition and delegates to EnforceStopOnRamCleanupFailure() to halt
     *          the application in a safe state.
     * @pre     A buffer deletion operation has failed.
     * @post    EnforceStopOnRamCleanupFailure() has been invoked;
     *          isRunning_ == false.
     * @throws  None.
     * @note    Called by: ProbeApp on buffer deletion failure.
     * @warning This is a safety-critical path. The application will stop processing
     *          after this call.
     * @requirements SWR-REQ-01-08-003
     * @see     EnforceStopOnRamCleanupFailure, DeleteTransmittedSteadyStateData
     */
    void HandleRamDeletionFailure() noexcept;

private:

    // =========================================================================
    // Collaborator Pointers (non-owning observers)
    // =========================================================================

    ProbeComm* probeComm_{nullptr};
    ///< @brief Non-owning observer pointer to the ProbeComm communication interface.
    ///         Must not be nullptr after construction. Lifetime managed by caller.

    ProbeCommVariant* probeCommVariant_{nullptr};
    ///< @brief Non-owning observer pointer to the ProbeCommVariant strategy object.
    ///         Determines variant-specific communication behaviour. Must not be nullptr.

    CanConsumer* canConsumer_{nullptr};
    ///< @brief Non-owning observer pointer to the CanConsumer shared-memory reader.
    ///         Used to read CAN signals at 100 ms and 1000 ms rates. Must not be nullptr.

    // =========================================================================
    // CAN Signal Ring Buffers
    // =========================================================================

    std::vector<std::vector<uint8_t>> canBuffer100ms_null;
    ///< @brief Ring buffer storing timestamped CAN signal snapshots sampled at 100 ms.
    ///         Each inner vector contains one serialised CAN data frame with timestamp.
    ///         Protected by bufferMutex100ms_.

    std::vector<std::vector<uint8_t>> canBuffer1000ms_null;
    ///< @brief Ring buffer storing timestamped CAN signal snapshots sampled at 1000 ms.
    ///         Each inner vector contains one serialised CAN data frame with timestamp.
    ///         Protected by bufferMutex1000ms_.

    // =========================================================================
    // Log Data Set Buffer
    // =========================================================================

    std::vector<std::vector<uint8_t>> logDataSetBuffer_null;
    ///< @brief Rolling buffer of per-second assembled log data sets.
    ///         Size is bounded by pastDataRetentionCount_. Protected by logBufferMutex_.

    // =========================================================================
    // Steady-State Transmission Buffers
    // =========================================================================

    std::vector<std::vector<uint8_t>> steadyStateDataBuffer100ms_null;
    ///< @brief Staging buffer for 100 ms steady-state data pending transmission to DAQ.
    ///         Cleared by ClearSteadyStateBuffer() or DeleteTransmittedSteadyStateData().

    std::vector<std::vector<uint8_t>> steadyStateDataBuffer1000ms_null;
    ///< @brief Staging buffer for 1000 ms steady-state data pending transmission to DAQ.
    ///         Cleared by ClearSteadyStateBuffer() or DeleteTransmittedSteadyStateData().

    // =========================================================================
    // Event and Image Buffers
    // =========================================================================

    std::vector<std::vector<uint8_t>> eventSendingBuffer_null;
    ///< @brief Buffer holding event-triggered data sets awaiting transmission.
    ///         Populated by StoreEventSignalData() and FillZeroDataForMissingPreTrigger().

    std::vector<uint8_t> imageDataBuffer_null;
    ///< @brief Buffer holding raw image data acquired during an event trigger.
    ///         Populated by image acquisition callbacks via ProbeComm.

    // =========================================================================
    // Synchronisation Primitives
    // =========================================================================

    mutable std::mutex bufferMutex100ms_null;
    ///< @brief Mutex protecting canBuffer100ms_ and steadyStateDataBuffer100ms_.
    ///         Must be acquired before any read or write to those buffers.

    mutable std::mutex bufferMutex1000ms_null;
    ///< @brief Mutex protecting canBuffer1000ms_ and steadyStateDataBuffer1000ms_.
    ///         Must be acquired before any read or write to those buffers.

    mutable std::mutex logBufferMutex_null;
    ///< @brief Mutex protecting logDataSetBuffer_ and eventSendingBuffer_.
    ///         Must be acquired before any read or write to those buffers.

    // =========================================================================
    // Lifecycle and Timing State
    // =========================================================================

    std::atomic<bool> isRunning_{false};
    ///< @brief Indicates whether the ProbeApp processing loop is active.
    ///         Set to true by HandleInitialize(); set to false by HandleTerminate()
    ///         or HandleShutdown(). Atomic to allow safe inspection from diagnostics.

    std::atomic<bool> shutdownRequested_{false};
    ///< @brief Indicates that a shutdown has been requested.
    ///         When true, Run() is a no-op and all buffers will be cleared.

    std::atomic<bool> isDaqCommEstablished_{false};
    ///< @brief Indicates whether SOME/IP communication with DAQ has been established.
    ///         Set to true when all required services are discovered and available.

    bool collectionStarted_{false};
    ///< @brief Indicates whether the 10-second startup delay has elapsed and data
    ///         collection has commenced. Set to true by CheckCollectionStartDelay().

    uint32_t msCounter_{0U};
    ///< @brief Millisecond counter incremented each Run() call (10 ms resolution).
    ///         Used to derive 100 ms and 1000 ms cycle triggers. Range: [0, UINT32_MAX].

    uint32_t cycleCounter_{0U};
    ///< @brief Steady-state transmit cadence counter for SendRegularData (100 ms vs 1000 ms).

    uint32_t startupTimeSec_{0U};
    ///< @brief Elapsed time in seconds since HandleInitialize() was called.
    ///         Used by CheckCollectionStartDelay() to enforce the 10-second startup delay.
    ///         Range: [0, UINT32_MAX].

    uint32_t pastDataRetentionCount_{0U};
    ///< @brief Maximum number of per-second log data sets to retain in logDataSetBuffer_.
    ///         Determines the pre-trigger data retention window depth.
    ///         Range: [1, UINT32_MAX]. Configured at construction from variant settings.

    // =========================================================================
    // Constants
    // =========================================================================

    /// @brief Mandatory startup delay in seconds before data collection may begin.
    /// @details Set to 10 seconds per SWR-REQ-01-10-003 to allow all ECU subsystems
    ///          and SOME/IP services to reach a stable operational state after IG-ON.
    static constexpr uint32_t kStartupDelaySeconds{10U};

    /// @brief Cyclic task period in milliseconds.
    /// @details ProbeApp::Run() is called from a 10 ms periodic task. This constant
    ///          is used to derive sub-cycle counters for 100 ms and 1000 ms operations.
    static constexpr uint32_t kTaskPeriodMs{10U};

    /// @brief Millisecond counter threshold for 100 ms cycle trigger.
    /// @details Every kCycle100msThreshold calls to Run() (i.e., every 100 ms),
    ///          the 100 ms CAN data store and steady-state send operations are triggered.
    static constexpr uint32_t kCycle100msThreshold{10U};

    /// @brief Millisecond counter threshold for 1000 ms cycle trigger.
    /// @details Every kCycle1000msThreshold calls to Run() (i.e., every 1000 ms),
    ///          the 1000 ms CAN data store, log data set assembly, and long-cycle
    ///          steady-state send operations are triggered.
    static constexpr uint32_t kCycle1000msThreshold{100U};
};

} // namespace probe