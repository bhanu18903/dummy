#pragma once
/**
 * @file   ProbeApp.hpp
 * @brief  Probe application component for CAN data collection, event-triggered logging, and DAQ transmission.
 * @details ProbeApp is the central application component responsible for periodic CAN signal
 *          collection at 100ms and 1000ms rates, event-based signal storage, log data set assembly,
 *          steady-state data transmission to DAQ via SOME/IP, and lifecycle management including
 *          initialization, shutdown, and RAM cleanup. It coordinates with ProbeComm for all
 *          external communication and enforces data retention policies and startup delay constraints.
 * @author  Engineering Team
 * @date    2024-01-01
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <optional>

#include "../ProbeComm/ProbeComm.hpp"

/**
 * @defgroup ProbeApplication Probe Application Components
 * @brief Components responsible for CAN data collection, event logging, and DAQ transmission.
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

/* Forward declarations for dependent types */
class ProbeCommVariant;
class CanConsumer;

/**
 * @class ProbeApp
 * @brief Central probe application managing data collection, buffering, and transmission to DAQ.
 * @details ProbeApp orchestrates the full lifecycle of probe data handling:
 *          - Periodic CAN signal collection at 100ms and 1000ms sampling rates
 *          - Event-based signal storage with timestamps and metadata
 *          - Per-second log data set assembly with rolling retention
 *          - Steady-state data transmission to DAQ via SOME/IP UDP fire-and-forget
 *          - Zero-fill of missing pre-trigger data after IG-ON
 *          - Controlled shutdown with complete RAM cleanup
 *
 *          Thread-safety: All shared ring buffers are protected by dedicated mutexes.
 *          The class assumes single-threaded cyclic invocation from the main task but
 *          protects internal buffers for safe concurrent access where needed.
 *
 *          Lifecycle: Constructed at startup, initialized via HandleInitialize(), runs
 *          cyclically via Run(), and is terminated/shut down via HandleTerminate()/HandleShutdown().
 *
 * @ingroup ProbeApplication
 * @note Compliant with MISRA C++ guidelines, ISO 26262 ASIL-B, and project coding standards.
 *       All variables initialized at declaration. No dynamic memory beyond std::vector buffers.
 *       No recursion. All parameters validated before use.
 * @warning Calling Run() before HandleInitialize() results in undefined application behavior.
 *          HandleShutdown() deletes all temporary data including mid-transmission data irreversibly.
 * @invariant bufferMutex100ms_ protects canBuffer100ms_ and steadyStateDataBuffer100ms_.
 *            bufferMutex1000ms_ protects canBuffer1000ms_ and steadyStateDataBuffer1000ms_.
 *            logBufferMutex_ protects logDataSetBuffer_.
 *            isRunning_ is only modified by HandleInitialize(), HandleTerminate(), and HandleShutdown().
 * @see ProbeComm, ProbeCommVariant, CanConsumer
 */
class ProbeApp
{
public:
    /**
     * @brief Constructs ProbeApp with references to communication, variant, and CAN consumer.
     * @details Initializes all internal state, ring buffers, counters, and flags to their
     *          default values. Stores non-owning pointers to the communication layer,
     *          variant configuration, and CAN consumer for use throughout the application lifecycle.
     *          No communication is initiated during construction.
     * @param[in] comm       Pointer to the ProbeComm communication interface. Must not be nullptr.
     *                        Lifetime must exceed that of this ProbeApp instance.
     * @param[in] variant    Pointer to the ProbeCommVariant configuration. Must not be nullptr.
     *                        Lifetime must exceed that of this ProbeApp instance.
     * @param[in] canCons    Pointer to the CanConsumer for CAN signal access. Must not be nullptr.
     *                        Lifetime must exceed that of this ProbeApp instance.
     * @throws std::invalid_argument If any parameter is nullptr.
     * @pre All three pointers must be valid and non-null.
     * @post ProbeApp is in a constructed but not yet initialized state. isRunning_ == false.
     * @note Called by: Main at startup.
     * @warning None of the pointed-to objects may be destroyed before this ProbeApp instance.
     * @see HandleInitialize
     * @requirements SWR-REQ-01-01-002, SWR-REQ-01-01-003, SWR-REQ-01-02-001
     * @rationale Constructor injection enables testability and decouples ProbeApp from
     *            concrete communication and CAN implementations.
     */
    explicit ProbeApp(ProbeComm* comm, ProbeCommVariant* variant, CanConsumer* canCons);

    /**
     * @brief Starts ProbeApp operation after ADAS ECU startup and initiates SOME/IP communication with DAQ.
     * @details Performs post-construction initialization including SOME/IP service discovery
     *          for DAQ communication channels, offering probe services, and setting the
     *          application into the running state. Must be called exactly once after construction
     *          and before Run().
     * @throws std::runtime_error If communication initialization fails.
     * @pre ProbeApp must be constructed. HandleInitialize() must not have been called previously.
     * @post isRunning_ == true. DAQ communication channels are being discovered.
     *       isDaqCommEstablished_ may still be false until service discovery completes.
     * @note Called from Main after construction.
     * @warning Blocking until initial service discovery requests are dispatched.
     * @see Run, HandleTerminate, HandleShutdown
     * @requirements SWR-REQ-01-10-001, SWR-REQ-01-10-002, SWR-REQ-01-03-001, SWR-REQ-03-16-001, SWR-REQ-03-16-002
     * @rationale Separating initialization from construction allows the caller to control
     *            when communication begins relative to other system startup activities.
     */
    void HandleInitialize();

    /**
     * @brief Runs the cyclic data collection and transmission processing loop.
     * @details Executes one iteration of the main processing cycle including:
     *          - Checking collection start delay
     *          - Collecting shared memory signals
     *          - Storing CAN data at 100ms and 1000ms rates
     *          - Building per-second log data sets
     *          - Sending regular steady-state data to DAQ
     *          - Deleting transmitted data
     *          This method is intended to be called cyclically from the main task scheduler.
     * @throws std::runtime_error If a critical processing error occurs.
     * @pre HandleInitialize() must have been called. isRunning_ == true.
     * @post One processing cycle is complete. Buffers and counters are updated accordingly.
     * @note Called cyclically from Main.
     * @warning Must not be called concurrently from multiple threads.
     * @see HandleInitialize, HandleTerminate
     * @requirements SWR-REQ-01-001, SWR-REQ-01-002, SWR-REQ-01-005, SWR-REQ-01-06-001, SWR-REQ-01-10-003, SWR-REQ-03-16-003
     * @rationale Single cyclic entry point simplifies scheduling and ensures deterministic execution order.
     */
    void Run();

    /**
     * @brief Stops all data collection and transmission processing on termination.
     * @details Sets the running flag to false, ceasing all cyclic processing in subsequent
     *          Run() calls. Does not delete buffered data — use HandleShutdown() for full cleanup.
     * @pre isRunning_ == true.
     * @post isRunning_ == false. No further data collection or transmission occurs.
     * @throws None.
     * @note Called from Main on termination request.
     * @warning Does not clear buffers. Data remains in memory until HandleShutdown() or destruction.
     * @see HandleShutdown, HandleInitialize
     * @requirements SWR-REQ-01-09-001, SWR-REQ-03-13-001
     * @rationale Allows graceful stop without data loss, enabling potential resume or inspection.
     */
    void HandleTerminate() noexcept;

    /**
     * @brief Stops all processing and deletes all temporary data on ADAS ECU shutdown.
     * @details Stops the running state and invokes ClearAllTemporaryData() to remove all
     *          buffered data from RAM including mid-transmission data. If RAM cleanup fails,
     *          EnforceStopOnRamCleanupFailure() is invoked to halt the application.
     * @pre ProbeApp must be in a constructed state (initialized or not).
     * @post isRunning_ == false. All buffers are cleared. shutdownRequested_ == true.
     * @throws None.
     * @note Called from Main on ECU shutdown.
     * @warning Irreversibly deletes all temporary data including partially transmitted event data.
     * @see HandleTerminate, ClearAllTemporaryData, EnforceStopOnRamCleanupFailure
     * @requirements SWR-REQ-01-09-001, SWR-REQ-01-09-002, SWR-REQ-03-13-001, SWR-REQ-03-13-002, SWR-REQ-03-13-003, SWR-REQ-01-08-003
     * @rationale Full cleanup on shutdown prevents stale data from persisting across drive cycles.
     */
    void HandleShutdown() noexcept;

    /**
     * @brief Reads CAN and internal signals from shared memory at configured sampling rates.
     * @details Accesses the shared memory region via the CanConsumer interface to read the
     *          latest CAN signal values and internal signals. Dispatches to StoreCanData100ms()
     *          and StoreCanData1000ms() based on the current millisecond counter.
     * @pre isRunning_ == true. collectionStarted_ == true. canConsumer_ must be valid.
     * @post Signal values are read and stored into the appropriate ring buffers.
     * @throws None.
     * @note Called internally by Run().
     * @warning Assumes shared memory is mapped and accessible. No validation of memory mapping.
     * @see StoreCanData100ms, StoreCanData1000ms, Run
     * @requirements SWR-REQ-01-01-001, SWR-REQ-01-001, SWR-REQ-01-002, SWR-REQ-01-003
     * @rationale Centralizes signal reading to ensure consistent sampling across all rates.
     */
    void CollectSharedMemReadSignals();

    /**
     * @brief Reads CAN signals at 100ms rate and stores them into the 100ms ring buffer with timestamp.
     * @details Acquires bufferMutex100ms_, reads the current CAN signal set from the CanConsumer,
     *          appends the data with a timestamp to canBuffer100ms_, and releases the mutex.
     * @pre canConsumer_ must be valid. bufferMutex100ms_ must not be held by the caller.
     * @post A new timestamped entry is appended to canBuffer100ms_.
     * @throws None.
     * @note Called internally by CollectSharedMemReadSignals() every 100ms.
     * @warning Acquires bufferMutex100ms_. Do not call while holding this mutex.
     * @see CollectSharedMemReadSignals, StoreCanData1000ms
     * @requirements SWR-REQ-01-001, SWR-REQ-01-01-001, SWR-REQ-01-01-002, SWR-REQ-01-003
     * @rationale Separate 100ms storage allows independent buffer management and transmission timing.
     */
    void StoreCanData100ms();

    /**
     * @brief Reads CAN signals at 1000ms rate and stores them into the 1000ms ring buffer with timestamp.
     * @details Acquires bufferMutex1000ms_, reads the current CAN signal set from the CanConsumer,
     *          appends the data with a timestamp to canBuffer1000ms_, and releases the mutex.
     * @pre canConsumer_ must be valid. bufferMutex1000ms_ must not be held by the caller.
     * @post A new timestamped entry is appended to canBuffer1000ms_.
     * @throws None.
     * @note Called internally by CollectSharedMemReadSignals() every 1000ms.
     * @warning Acquires bufferMutex1000ms_. Do not call while holding this mutex.
     * @see CollectSharedMemReadSignals, StoreCanData100ms
     * @requirements SWR-REQ-01-002, SWR-REQ-01-01-001, SWR-REQ-01-01-002, SWR-REQ-01-003
     * @rationale Separate 1000ms storage allows independent buffer management and transmission timing.
     */
    void StoreCanData1000ms();

    /**
     * @brief Collects event-based signals with timestamps, signal IDs, and data size information.
     * @details Reads event-triggered signal data from the CanConsumer, attaches metadata
     *          (timestamp, signal ID, data size), and stores the result in the event sending buffer.
     *          Acquires logBufferMutex_ to protect shared event data structures.
     * @pre isRunning_ == true. collectionStarted_ == true.
     * @post Event signal data with metadata is stored in eventSendingBuffer_.
     * @throws None.
     * @note Called internally by Run().
     * @warning Acquires logBufferMutex_. Do not call while holding this mutex.
     * @see BuildLogDataSetPerSecond, Run
     * @requirements SWR-REQ-03-01-002, SWR-REQ-03-01-004, SWR-REQ-03-01-005, SWR-REQ-03-01-006, SWR-REQ-03-01-007
     * @rationale Event-based collection captures transient signals that periodic sampling would miss.
     */
    void StoreEventSignalData();

    /**
     * @brief Assembles per-second log data sets per the collection data retention layout specification.
     * @details Compiles all collected signals (CAN and event) accumulated during the last second
     *          into a single log data set entry. Acquires logBufferMutex_ and appends the assembled
     *          data set to logDataSetBuffer_. Invokes DeleteOldestLogDataSet() if the retention
     *          count is exceeded.
     * @pre Sufficient signal data has been collected for the current second.
     * @post A new log data set is appended to logDataSetBuffer_. Oldest entry may be removed.
     * @throws None.
     * @note Called internally by Run() at 1-second intervals.
     * @warning Acquires logBufferMutex_. Do not call while holding this mutex.
     * @see DeleteOldestLogDataSet, StoreEventSignalData
     * @requirements SWR-REQ-03-01-008, SWR-REQ-03-001
     * @rationale Per-second assembly aligns with the data retention layout specification and
     *            enables efficient rolling buffer management.
     */
    void BuildLogDataSetPerSecond();

    /**
     * @brief Removes the oldest log data set when the retention count is exceeded.
     * @details Checks if logDataSetBuffer_ size exceeds pastDataRetentionCount_ and removes
     *          the oldest entry (front of buffer) if so. Acquires logBufferMutex_.
     * @pre logBufferMutex_ must not be held by the caller.
     * @post logDataSetBuffer_.size() <= pastDataRetentionCount_ (if it was exceeded).
     * @throws None.
     * @note Called internally by BuildLogDataSetPerSecond().
     * @warning Acquires logBufferMutex_. Do not call while holding this mutex.
     * @see BuildLogDataSetPerSecond
     * @requirements SWR-REQ-03-01-009, SWR-REQ-03-001
     * @rationale Rolling deletion ensures bounded memory usage for log data retention.
     */
    void DeleteOldestLogDataSet();

    /**
     * @brief Sends zero-filled data blocks for pre-trigger time window where log data is not yet available.
     * @details After IG-ON, there may be a period where log data has not yet been collected.
     *          This method fills that gap with zero-valued data blocks so that the complete
     *          pre-trigger time window is represented in the transmitted data.
     * @param[in] startTime The start time of the pre-trigger window in seconds relative to IG-ON.
     *                       Valid range: negative values representing seconds before trigger.
     * @pre A trigger event has been received. startTime is within the valid retention window.
     * @post Zero-filled data blocks are generated for the missing pre-trigger period.
     * @throws None.
     * @note Called by ProbeApp during event trigger processing.
     * @warning May generate significant data volume if the missing window is large.
     * @see BuildLogDataSetPerSecond
     * @requirements SWR-REQ-03-002, SWR-REQ-03-01-011, SWR-REQ-03-07-007
     * @rationale Ensures complete data coverage for the pre-trigger window even when the
     *            system has recently started and historical data is unavailable.
     */
    void FillZeroDataForMissingPreTrigger(int32_t startTime);

    /**
     * @brief Deletes steady-state data from RAM after successful transmission to DAQ.
     * @details Clears steadyStateDataBuffer100ms_ and steadyStateDataBuffer1000ms_ after
     *          their contents have been successfully transmitted. Acquires the appropriate
     *          buffer mutexes during clearing.
     * @pre Steady-state data has been transmitted successfully.
     * @post steadyStateDataBuffer100ms_ and steadyStateDataBuffer1000ms_ are empty.
     * @throws None.
     * @note Called internally after successful SendRegularData().
     * @warning Acquires bufferMutex100ms_ and bufferMutex1000ms_. Do not call while holding these.
     * @see SendRegularData, DeleteTransmittedSteadyStateData
     * @requirements SWR-REQ-01-006, SWR-REQ-01-07-001
     * @rationale Prompt deletion of transmitted data frees RAM for new data collection.
     */
    void ClearSteadyStateBuffer();

    /**
     * @brief Clears all RAM buffers including regular, event, image, and mid-transmission data.
     * @details Acquires all buffer mutexes and clears canBuffer100ms_, canBuffer1000ms_,
     *          logDataSetBuffer_, steadyStateDataBuffer100ms_, steadyStateDataBuffer1000ms_,
     *          eventSendingBuffer_, and imageDataBuffer_. Used during shutdown to ensure
     *          no stale data remains in memory.
     * @pre None — safe to call in any state.
     * @post All data buffers are empty.
     * @throws None.
     * @note Called internally by HandleShutdown().
     * @warning Acquires all buffer mutexes. Do not call while holding any buffer mutex.
     *          Irreversibly deletes all buffered data.
     * @see HandleShutdown, EnforceStopOnRamCleanupFailure
     * @requirements SWR-REQ-01-09-002, SWR-REQ-03-13-002
     * @rationale Complete RAM cleanup on shutdown prevents data leakage across drive cycles.
     */
    void ClearAllTemporaryData();

    /**
     * @brief Verifies that data collection can start after the 10-second delay from IG-ON.
     * @details Compares the elapsed time since IG-ON (tracked by startupTimeSec_) against
     *          the required 10-second startup delay. Returns true if the delay has elapsed
     *          and sets collectionStarted_ to true on first successful check.
     * @return True if the 10-second startup delay has elapsed and collection may begin.
     * @retval true  The startup delay has elapsed; data collection is permitted.
     * @retval false The startup delay has not yet elapsed; data collection must wait.
     * @pre isRunning_ == true. startupTimeSec_ is being incremented.
     * @post If true is returned, collectionStarted_ == true.
     * @throws None.
     * @note Called by ProbeApp at the beginning of each Run() cycle.
     * @warning Returns false for the first 10 seconds after IG-ON regardless of other state.
     * @see Run, HandleInitialize
     * @requirements SWR-REQ-01-10-003, SWR-REQ-03-01-001, SWR-REQ-03-16-003
     * @rationale The 10-second delay ensures all ECU subsystems are stable before data collection begins.
     */
    bool CheckCollectionStartDelay() noexcept;

    /**
     * @brief Halts application if RAM cleanup fails to prevent inconsistent state.
     * @details Checks the result of ClearAllTemporaryData() and, if any buffer was not
     *          successfully cleared, sets isRunning_ to false and logs the failure condition.
     *          This prevents the application from operating with inconsistent buffer state.
     * @pre ClearAllTemporaryData() has been attempted.
     * @post If cleanup failed: isRunning_ == false. Application is in a halted state.
     * @throws None.
     * @note Called internally by HandleShutdown() after ClearAllTemporaryData().
     * @warning Halts the entire ProbeApp. Recovery requires full restart.
     * @see ClearAllTemporaryData, HandleShutdown, HandleRamDeletionFailure
     * @requirements SWR-REQ-01-08-003, SWR-REQ-03-13-003
     * @rationale Fail-stop behavior on cleanup failure is required by ISO 26262 ASIL-B
     *            to prevent operation with corrupted or inconsistent data.
     */
    void EnforceStopOnRamCleanupFailure() noexcept;

    /**
     * @brief Transmits steady-state sampled data to DAQ via SOME/IP UDP fire-and-forget.
     * @details Sends the contents of steadyStateDataBuffer100ms_ at 100ms intervals and
     *          steadyStateDataBuffer1000ms_ at 1000ms intervals via ProbeComm's
     *          SendContinualShortDataCyclic100ms() and SendContinualLongDataCyclic1000ms()
     *          methods respectively. Checks isDaqCommEstablished_ before transmission.
     * @pre isRunning_ == true. isDaqCommEstablished_ == true for transmission to proceed.
     * @post Steady-state data has been dispatched to ProbeComm for transmission.
     * @throws None.
     * @note Called internally by Run() at the appropriate cyclic intervals.
     * @warning Data is sent fire-and-forget; no delivery confirmation is received.
     * @see ClearSteadyStateBuffer, DeleteTransmittedSteadyStateData
     * @requirements SWR-REQ-01-005, SWR-REQ-01-06-001, SWR-REQ-01-06-002, SWR-REQ-01-08-001, SWR-REQ-01-08-002
     * @rationale Fire-and-forget via UDP minimizes latency for steady-state telemetry data.
     */
    void SendRegularData();

    /**
     * @brief Removes transmitted steady-state data from RAM buffer after transmission.
     * @details Clears the steady-state data that has been sent (or attempted) from the
     *          100ms and 1000ms buffers. Called after SendRegularData() regardless of
     *          transmission success or failure to prevent buffer overflow.
     * @pre SendRegularData() has been called for the current cycle.
     * @post Transmitted steady-state data entries are removed from the buffers.
     * @throws None.
     * @note Called internally after SendRegularData().
     * @warning Data is deleted even if transmission failed (fire-and-forget semantics).
     * @see SendRegularData, ClearSteadyStateBuffer, HandleRamDeletionFailure
     * @requirements SWR-REQ-01-006, SWR-REQ-01-07-001, SWR-REQ-01-08-002
     * @rationale Unconditional deletion after send prevents unbounded buffer growth
     *            in fire-and-forget transmission mode.
     */
    void DeleteTransmittedSteadyStateData();

    /**
     * @brief Initiates controlled stop if RAM buffer deletion fails after communication error.
     * @details Detects that a RAM buffer deletion operation has failed and triggers
     *          EnforceStopOnRamCleanupFailure() to halt the application. This prevents
     *          continued operation with potentially corrupted buffer state.
     * @pre A RAM deletion operation has failed.
     * @post isRunning_ == false if the failure is confirmed.
     * @throws None.
     * @note Called internally when DeleteTransmittedSteadyStateData() or ClearAllTemporaryData() fails.
     * @warning Halts the entire ProbeApp. Recovery requires full restart.
     * @see EnforceStopOnRamCleanupFailure, DeleteTransmittedSteadyStateData
     * @requirements SWR-REQ-01-08-003
     * @rationale Fail-stop on RAM deletion failure is a safety requirement to prevent
     *            data corruption propagation per ISO 26262 ASIL-B.
     */
    void HandleRamDeletionFailure() noexcept;

private:
    /* ===== Non-owning pointers to injected dependencies ===== */

    ProbeComm* probeComm_{nullptr};               ///< @brief Non-owning pointer to ProbeComm communication interface. Must outlive this instance.
    ProbeCommVariant* probeCommVariant_{nullptr};  ///< @brief Non-owning pointer to variant configuration. Must outlive this instance.
    CanConsumer* canConsumer_{nullptr};            ///< @brief Non-owning pointer to CAN signal consumer. Must outlive this instance.

    /* ===== CAN data ring buffers ===== */

    std::vector<std::vector<uint8_t>> canBuffer100ms_null;    ///< @brief Ring buffer for CAN signals sampled at 100ms rate. Protected by bufferMutex100ms_.
    std::vector<std::vector<uint8_t>> canBuffer1000ms_null;   ///< @brief Ring buffer for CAN signals sampled at 1000ms rate. Protected by bufferMutex1000ms_.

    /* ===== Log and event data buffers ===== */

    std::vector<std::vector<uint8_t>> logDataSetBuffer_null;            ///< @brief Rolling buffer of per-second log data sets. Protected by logBufferMutex_.
    std::vector<std::vector<uint8_t>> steadyStateDataBuffer100ms_null;  ///< @brief Buffer for 100ms steady-state data pending transmission. Protected by bufferMutex100ms_.
    std::vector<std::vector<uint8_t>> steadyStateDataBuffer1000ms_null; ///< @brief Buffer for 1000ms steady-state data pending transmission. Protected by bufferMutex1000ms_.
    std::vector<std::vector<uint8_t>> eventSendingBuffer_null;          ///< @brief Buffer for event-triggered signal data pending transmission.
    std::vector<uint8_t> imageDataBuffer_null;                          ///< @brief Buffer for camera image data pending transmission.

    /* ===== Mutexes for buffer protection ===== */

    std::mutex bufferMutex100ms_null;   ///< @brief Mutex protecting canBuffer100ms_ and steadyStateDataBuffer100ms_.
    std::mutex bufferMutex1000ms_null;  ///< @brief Mutex protecting canBuffer1000ms_ and steadyStateDataBuffer1000ms_.
    std::mutex logBufferMutex_null;     ///< @brief Mutex protecting logDataSetBuffer_ and event data structures.

    /* ===== Counters and configuration ===== */

    uint32_t msCounter_{0U};                ///< @brief Millisecond counter for cyclic timing. Incremented each Run() cycle. Range: [0, UINT32_MAX].
    uint32_t pastDataRetentionCount_{0U};   ///< @brief Maximum number of log data sets retained in the rolling buffer. Configured at initialization.
    uint32_t startupTimeSec_{0U};           ///< @brief Elapsed seconds since IG-ON. Used for startup delay check. Range: [0, UINT32_MAX].

    /* ===== State flags ===== */

    bool isRunning_{false};              ///< @brief True when the application is in the running state. Set by HandleInitialize(), cleared by HandleTerminate()/HandleShutdown().
    bool isDaqCommEstablished_{false};   ///< @brief True when SOME/IP communication with DAQ is established. Set after successful service discovery.
    bool shutdownRequested_{false};      ///< @brief True when a shutdown has been requested. Set by HandleShutdown().
    bool collectionStarted_{false};      ///< @brief True when the 10-second startup delay has elapsed and data collection has begun.
};

}  // namespace probe