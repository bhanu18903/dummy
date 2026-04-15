#pragma once
/**
 * @file   CanConsumer.hpp
 * @brief  CAN bus data consumer for reading RX/TX signals from shared memory.
 * @details Provides the CanConsumer class which interfaces with a shared memory
 *          region to read CAN bus signals at configurable periodicities (100ms, 1000ms).
 *          Supports individual signal reads by ID, timestamp collection, and data validation.
 *          Thread-safe access to shared memory is protected via std::mutex.
 * @author  Engineering Team
 * @date    2024-01-01
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <map>
#include <optional>

/**
 * @defgroup CanDataAcquisition CAN Data Acquisition Components
 * @brief Components responsible for reading and validating CAN bus signal data
 *        from shared memory regions.
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

/**
 * @class CanConsumer
 * @brief Consumes CAN bus RX/TX signal data from a shared memory region.
 * @details The CanConsumer class is responsible for reading CAN bus signals from
 *          a shared memory handle provided at construction time. It supports two
 *          cyclic read periodicities (100ms and 1000ms), individual signal reads
 *          by signal ID for both RX and TX directions, timestamp collection at
 *          signal acquisition time, and validation of collected signal data.
 *
 *          The class follows RAII principles: the shared memory handle is stored
 *          as a non-owning observer pointer whose lifetime is managed externally
 *          by the caller (ProbeApp). All shared state is protected by a mutex
 *          to ensure thread-safe operation.
 *
 * @ingroup CanDataAcquisition
 *
 * @note Compliant with MISRA C++:2008, AUTOSAR C++14 coding guidelines,
 *       and ISO 26262 ASIL-B functional safety requirements. All variables are
 *       initialized at declaration. No dynamic memory allocation beyond
 *       std::vector returns. No recursion is used.
 *
 * @warning The shared memory handle (memHandle) must remain valid for the entire
 *          lifetime of this object. Passing a null or dangling pointer results
 *          in undefined behavior if not caught at construction.
 *
 * @invariant sharedMemHandle_ is either nullptr (invalid state) or points to a
 *            valid shared memory region. The mutex memMutex_ must be held during
 *            all shared memory read operations.
 *
 * @see ProbeApp (caller that constructs and invokes cyclic reads)
 */
class CanConsumer final
{
public:
    /**
     * @brief Constructs a CanConsumer with a shared memory handle for signal reading.
     * @details Initializes the CanConsumer instance by storing the provided shared
     *          memory handle and setting all internal timestamp members to zero.
     *          The handle is treated as a non-owning observer pointer; the caller
     *          is responsible for ensuring the pointed-to memory remains valid
     *          for the lifetime of this object.
     *
     * @param[in] memHandle Pointer to the shared memory region used for CAN signal
     *                      reading. Must not be nullptr. Valid range: any non-null
     *                      pointer to a mapped shared memory region.
     *
     * @throws std::invalid_argument If memHandle is nullptr.
     *
     * @pre The shared memory region pointed to by memHandle must be initialized
     *      and mapped before constructing this object.
     * @post The CanConsumer is ready to perform signal reads. Internal timestamps
     *       are initialized to zero.
     *
     * @note Called by: ProbeApp at startup.
     * @warning The caller must guarantee that the shared memory region outlives
     *          this CanConsumer instance.
     *
     * @see ProbeApp
     * @requirements SWR-REQ-01-01-001
     * @rationale Shared memory handle is accepted as void* to decouple from the
     *            specific shared memory implementation type.
     */
    explicit CanConsumer(void* memHandle);

    /**
     * @brief Default destructor.
     * @details Releases no resources as the shared memory handle is non-owning.
     *
     * @throws None.
     *
     * @post Object is destroyed. No shared memory cleanup is performed.
     */
    ~CanConsumer() noexcept = default;

    /** @brief Deleted copy constructor to enforce single ownership semantics. */
    CanConsumer(const CanConsumer&) = delete;

    /** @brief Deleted copy assignment operator to enforce single ownership semantics. */
    CanConsumer& operator=(const CanConsumer&) = delete;

    /** @brief Deleted move constructor to prevent transfer of shared memory handle. */
    CanConsumer(CanConsumer&&) = delete;

    /** @brief Deleted move assignment operator to prevent transfer of shared memory handle. */
    CanConsumer& operator=(CanConsumer&&) = delete;

    /**
     * @brief Reads CAN RX/TX signals from shared memory for 100ms periodicity.
     * @details Iterates over all configured signal IDs in the signal ID map,
     *          reads both RX and TX data for each signal, validates the collected
     *          data, and collects a timestamp at acquisition time. Returns the
     *          aggregated signal data as a byte vector. Thread-safe: acquires
     *          memMutex_ for the duration of the read operation.
     *
     * @return std::vector<uint8_t> Byte vector containing the aggregated CAN
     *         signal data read at 100ms periodicity. Returns an empty vector
     *         if no valid data is available or an error occurs.
     *
     * @retval empty_vector No valid signal data was available or all reads failed.
     * @retval non_empty_vector Successfully read and validated CAN signal data.
     *
     * @throws None. Errors are indicated by returning an empty vector.
     *
     * @pre The CanConsumer must have been successfully constructed with a valid
     *      shared memory handle.
     * @post Internal timestamps (timestampSec_, timestampMsec_) are updated to
     *       reflect the time of the most recent signal acquisition.
     *
     * @note Called by: ProbeApp at Cyclic_100ms.
     * @warning This function acquires a mutex lock. Do not call from an
     *          interrupt context or while holding memMutex_ externally.
     *
     * @see ReadCanRxData, ReadCanTxData, CollectTimestamp, ValidateSignalData
     * @requirements SWR-REQ-01-001, SWR-REQ-01-01-001
     * @rationale Separate 100ms read function allows independent scheduling from
     *            the 1000ms read cycle, enabling different signal groups per rate.
     */
    std::vector<uint8_t> ReadCanData100ms();

    /**
     * @brief Reads CAN RX/TX signals from shared memory for 1000ms periodicity.
     * @details Iterates over all configured signal IDs in the signal ID map,
     *          reads both RX and TX data for each signal, validates the collected
     *          data, and collects a timestamp at acquisition time. Returns the
     *          aggregated signal data as a byte vector. Thread-safe: acquires
     *          memMutex_ for the duration of the read operation.
     *
     * @return std::vector<uint8_t> Byte vector containing the aggregated CAN
     *         signal data read at 1000ms periodicity. Returns an empty vector
     *         if no valid data is available or an error occurs.
     *
     * @retval empty_vector No valid signal data was available or all reads failed.
     * @retval non_empty_vector Successfully read and validated CAN signal data.
     *
     * @throws None. Errors are indicated by returning an empty vector.
     *
     * @pre The CanConsumer must have been successfully constructed with a valid
     *      shared memory handle.
     * @post Internal timestamps (timestampSec_, timestampMsec_) are updated to
     *       reflect the time of the most recent signal acquisition.
     *
     * @note Called by: ProbeApp at Cyclic_1000ms.
     * @warning This function acquires a mutex lock. Do not call from an
     *          interrupt context or while holding memMutex_ externally.
     *
     * @see ReadCanRxData, ReadCanTxData, CollectTimestamp, ValidateSignalData
     * @requirements SWR-REQ-01-002, SWR-REQ-01-01-001
     * @rationale Separate 1000ms read function allows lower-frequency signals to
     *            be read independently, reducing unnecessary shared memory access.
     */
    std::vector<uint8_t> ReadCanData1000ms();

private:
    /**
     * @brief Reads a specific CAN RX signal from shared memory by signal ID.
     * @details Accesses the shared memory region using the provided signal ID to
     *          locate and read the corresponding CAN RX signal data. The caller
     *          must hold memMutex_ or ensure exclusive access before invoking
     *          this function.
     *
     * @param[in] signalId Unique identifier of the CAN RX signal to read.
     *                     Valid range: [0, UINT32_MAX]. Must correspond to a
     *                     valid signal entry in the shared memory region.
     *
     * @return std::vector<uint8_t> Byte vector containing the raw CAN RX signal
     *         data. Returns an empty vector if the signal ID is invalid or the
     *         read operation fails.
     *
     * @retval empty_vector Signal ID not found or read operation failed.
     * @retval non_empty_vector Successfully read CAN RX signal data.
     *
     * @throws None. Errors are indicated by returning an empty vector.
     *
     * @pre sharedMemHandle_ must be non-null and point to a valid shared memory
     *      region. The caller should hold memMutex_.
     * @post No side effects on internal state beyond the returned data.
     *
     * @note Called by: CanConsumer (internal use from ReadCanData100ms/1000ms).
     * @warning Caller must ensure thread-safe access to shared memory.
     *
     * @see ReadCanTxData, ReadCanData100ms, ReadCanData1000ms
     * @requirements SWR-REQ-01-01-001
     * @rationale Separated from TX reads to allow independent access patterns
     *            and clearer signal direction semantics.
     */
    std::vector<uint8_t> ReadCanRxData(const uint32_t signalId);

    /**
     * @brief Reads a specific CAN TX signal from shared memory by signal ID.
     * @details Accesses the shared memory region using the provided signal ID to
     *          locate and read the corresponding CAN TX signal data. The caller
     *          must hold memMutex_ or ensure exclusive access before invoking
     *          this function.
     *
     * @param[in] signalId Unique identifier of the CAN TX signal to read.
     *                     Valid range: [0, UINT32_MAX]. Must correspond to a
     *                     valid signal entry in the shared memory region.
     *
     * @return std::vector<uint8_t> Byte vector containing the raw CAN TX signal
     *         data. Returns an empty vector if the signal ID is invalid or the
     *         read operation fails.
     *
     * @retval empty_vector Signal ID not found or read operation failed.
     * @retval non_empty_vector Successfully read CAN TX signal data.
     *
     * @throws None. Errors are indicated by returning an empty vector.
     *
     * @pre sharedMemHandle_ must be non-null and point to a valid shared memory
     *      region. The caller should hold memMutex_.
     * @post No side effects on internal state beyond the returned data.
     *
     * @note Called by: CanConsumer (internal use from ReadCanData100ms/1000ms).
     * @warning Caller must ensure thread-safe access to shared memory.
     *
     * @see ReadCanRxData, ReadCanData100ms, ReadCanData1000ms
     * @requirements SWR-REQ-01-01-001
     * @rationale Separated from RX reads to allow independent access patterns
     *            and clearer signal direction semantics.
     */
    std::vector<uint8_t> ReadCanTxData(const uint32_t signalId);

    /**
     * @brief Captures seconds and milliseconds timestamp at the time of signal collection.
     * @details Reads the current system time and decomposes it into seconds and
     *          milliseconds components, storing them in timestampSec_ and
     *          timestampMsec_ respectively. Returns a combined 64-bit timestamp
     *          value representing the acquisition time. Uses a monotonic or
     *          steady clock source for deterministic timing.
     *
     * @return uint64_t Combined timestamp value in milliseconds since epoch.
     *         Range: [0, UINT64_MAX].
     *
     * @retval 0 Timestamp collection failed or clock source unavailable.
     * @retval non_zero Successfully captured timestamp at signal acquisition time.
     *
     * @throws None.
     *
     * @pre The system clock must be available and operational.
     * @post timestampSec_ and timestampMsec_ are updated with the current time
     *       components.
     *
     * @note Called by: CanConsumer (internal use during cyclic reads).
     * @warning Timestamp resolution depends on the underlying system clock.
     *          Not suitable for sub-millisecond timing requirements.
     *
     * @see ReadCanData100ms, ReadCanData1000ms
     * @requirements SWR-REQ-01-003, SWR-REQ-03-01-005
     * @rationale Timestamp is collected at signal acquisition time rather than
     *            at processing time to ensure accurate temporal correlation of
     *            CAN signal data.
     */
    uint64_t CollectTimestamp();

    /**
     * @brief Checks if the collected signal data is valid.
     * @details Performs validation checks on the provided signal data byte vector,
     *          including non-empty check, length verification, and data integrity
     *          checks. Returns true only if all validation criteria are satisfied.
     *
     * @param[in] data Byte vector containing the CAN signal data to validate.
     *                 Valid range: any std::vector<uint8_t>, including empty.
     *
     * @return bool Indicates whether the signal data passed all validation checks.
     *
     * @retval true  The signal data is valid and can be used for further processing.
     * @retval false The signal data is invalid (empty, corrupted, or out of range).
     *
     * @throws None.
     *
     * @pre None. This function accepts any input including empty vectors.
     * @post No modification to internal state.
     *
     * @note Called by: CanConsumer (internal use during cyclic reads).
     * @warning An empty data vector will always return false.
     *
     * @see ReadCanData100ms, ReadCanData1000ms, ReadCanRxData, ReadCanTxData
     * @requirements SWR-REQ-01-01-001
     * @rationale Validation is performed as a separate step to maintain single
     *            responsibility and allow reuse across different read functions.
     */
    bool ValidateSignalData(const std::vector<uint8_t>& data) const noexcept;

    /**
     * @brief Non-owning pointer to the shared memory region for CAN signal access.
     * @details Lifetime is managed by the caller (ProbeApp). Must remain valid
     *          for the entire lifetime of this CanConsumer instance.
     *          Set at construction, never reassigned. Range: non-null when valid.
     */
    void* sharedMemHandle_{nullptr}; ///< @brief Non-owning handle to shared memory region. Must not be nullptr after construction.

    /**
     * @brief Seconds component of the most recent signal acquisition timestamp.
     * @details Updated by CollectTimestamp() during each cyclic read.
     *          Range: [0, UINT32_MAX]. Units: seconds since epoch.
     */
    uint32_t timestampSec_{0U}; ///< @brief Seconds component of acquisition timestamp. Range: [0, UINT32_MAX].

    /**
     * @brief Milliseconds component of the most recent signal acquisition timestamp.
     * @details Updated by CollectTimestamp() during each cyclic read.
     *          Range: [0, 999]. Units: milliseconds within the current second.
     */
    uint32_t timestampMsec_{0U}; ///< @brief Milliseconds component of acquisition timestamp. Range: [0, 999].

    /**
     * @brief Map of signal identifiers to their shared memory offsets or metadata.
     * @details Used to iterate over configured signals during cyclic reads.
     *          Key: signal ID (uint32_t), Value: offset or configuration index (uint32_t).
     *          Populated during construction or configuration phase.
     */
    std::map<uint32_t, uint32_t> signalIdMap_null; ///< @brief Signal ID to shared memory offset mapping. Empty until configured.

    /**
     * @brief Mutex protecting all shared memory read operations.
     * @details Must be acquired before any access to sharedMemHandle_ to ensure
     *          thread-safe operation. Uses std::mutex for RAII-based locking.
     */
    mutable std::mutex memMutex_null; ///< @brief Mutex for thread-safe shared memory access.
};

} /* namespace probe */