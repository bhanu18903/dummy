/**
 * @file   CanConsumer.cpp
 * @brief  Implementation of the CAN bus data consumer for reading RX/TX signals from shared memory.
 * @details Implements all methods declared in CanConsumer.hpp. Provides cyclic signal
 *          acquisition at 100ms and 1000ms periodicities, individual RX/TX signal reads,
 *          timestamp collection using std::chrono steady clock, and signal data validation.
 *          All shared memory access is protected by std::mutex via std::lock_guard.
 * @author  Engineering Team
 * @date    2024-01-01
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include "CanConsumer.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <stdexcept>

/**
 * @ingroup CanDataAcquisition
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

/// @brief Maximum allowed CAN signal data size in bytes.
/// @details Derived from the standard CAN frame payload limit of 8 bytes.
///          CAN FD frames may extend to 64 bytes; this constant covers both.
///          Any signal data exceeding this size is considered corrupted.
static constexpr uint32_t kMaxSignalDataSize{64U};

/// @brief Default signal data size in bytes used when descriptor size is unavailable.
/// @details Set to 8 bytes corresponding to a standard CAN 2.0 frame payload.
static constexpr uint32_t kDefaultSignalDataSize{8U};

/// @brief Number of bits to shift seconds into the upper 32 bits of a uint64_t.
/// @details Used by CollectTimestamp to pack seconds and milliseconds into a
///          single 64-bit value: upper 32 bits = seconds, lower 32 bits = milliseconds.
static constexpr uint32_t kTimestampShiftBits{32U};

/// @brief Number of milliseconds per second, used for timestamp decomposition.
/// @details Value: 1000. Used to extract the milliseconds component from
///          a chrono duration.
static constexpr uint32_t kMillisecondsPerSecond{1000U};

/**
 * @brief Constructs a CanConsumer with a shared memory handle for signal reading.
 * @details Assigns the shared memory handle to the internal state, initializes
 *          timestamp fields and signal ID map to safe default values. Validates
 *          that the received handle is not null. If null, logs an error via
 *          std::cerr; subsequent reads will detect the null handle and return
 *          empty vectors. Ensures deterministic and bounded startup behavior.
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
 *       are initialized to zero. signalIdMap_ is empty.
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
CanConsumer::CanConsumer(void* memHandle)
    : sharedMemHandle_{memHandle},
      timestampSec_{0U},
      timestampMsec_{0U},
      signalIdMap_nullnull,
      memMutex_nullnull
{
    /* Validate that the received handle is not null */
    if (sharedMemHandle_ == nullptr)
    {
        /* Log error condition; handle is invalid */
        std::cerr << "[CanConsumer] ERROR: Null shared memory handle provided at construction. "
                  << "Unit enters degraded state; subsequent reads will return empty vectors."
                  << std::endl;

        /* Throw exception to notify caller of invalid construction parameter */
        throw std::invalid_argument("[CanConsumer] memHandle must not be nullptr");
    }
    else
    {
        /* Handle is valid; unit is ready for cyclic signal reads */
        std::cout << "[CanConsumer] INFO: Successfully initialized with valid shared memory handle."
                  << std::endl;
    }
}

/**
 * @brief Reads CAN RX/TX signals from shared memory for 100ms periodicity.
 * @details Iterates all 100ms rate signals, reads RX and TX data from shared
 *          memory, captures timestamp, validates each signal block, assembles
 *          result vector. Thread-safe: acquires memMutex_ via std::lock_guard
 *          for the duration of the read operation.
 *
 * @return std::vector<uint8_t> Byte vector containing the aggregated CAN
 *         signal data read at 100ms periodicity. Returns an empty vector
 *         if no valid data is available or an error occurs.
 *
 * @retval empty_vector No valid signal data was available, all reads failed,
 *                      or sharedMemHandle_ is null.
 * @retval non_empty_vector Successfully read and validated CAN signal data
 *                          with appended timestamp bytes.
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
std::vector<uint8_t> CanConsumer::ReadCanData100ms()
{
    /* Guard: verify shared memory handle is valid */
    if (sharedMemHandle_ == nullptr)
    {
        /* Return empty vector; caller will detect and log */
        std::cerr << "[CanConsumer] ERROR: ReadCanData100ms called with null shared memory handle."
                  << std::endl;
        return std::vector<uint8_t>null;
    }

    /* Acquire mutex for thread-safe shared memory access */
    const std::lock_guard<std::mutex> lock(memMutex_null);

    /* Capture acquisition timestamp before signal reads */
    const uint64_t timestamp = CollectTimestamp();
    static_cast<void>(timestamp); /* Timestamp stored in members; packed value available if needed */

    /* Initialize output accumulation buffer */
    std::vector<uint8_t> resultBuffernull;

    /* Iterate over all registered signal IDs in the 100ms group */
    for (const auto& entry : signalIdMap_null)
    {
        const uint32_t signalId = entry.first;

        /* Read RX signal data for this signal ID */
        const std::vector<uint8_t> rxData = ReadCanRxData(signalId);

        /* Read TX signal data for this signal ID */
        const std::vector<uint8_t> txData = ReadCanTxData(signalId);

        /* Validate received RX signal data */
        if (ValidateSignalData(rxData) == true)
        {
            /* Append valid RX data to result buffer */
            resultBuffer.insert(resultBuffer.end(), rxData.begin(), rxData.end());
        }
        else
        {
            /* Skip invalid RX entry; log signal ID and continue */
            std::cerr << "[CanConsumer] WARNING: ReadCanData100ms - Invalid RX data for signalId="
                      << signalId << std::endl;
        }

        /* Validate received TX signal data */
        if (ValidateSignalData(txData) == true)
        {
            /* Append valid TX data to result buffer */
            resultBuffer.insert(resultBuffer.end(), txData.begin(), txData.end());
        }
        else
        {
            /* Skip invalid TX entry; log signal ID and continue */
            std::cerr << "[CanConsumer] WARNING: ReadCanData100ms - Invalid TX data for signalId="
                      << signalId << std::endl;
        }
    }

    /* Append timestamp (sec and msec extracted from members) to result buffer */
    /* Append timestampSec_ as 4 bytes (little-endian) */
    const uint32_t secValue = timestampSec_;
    resultBuffer.push_back(static_cast<uint8_t>(secValue & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((secValue >> 8U) & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((secValue >> 16U) & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((secValue >> 24U) & 0xFFU));

    /* Append timestampMsec_ as 4 bytes (little-endian) */
    const uint32_t msecValue = timestampMsec_;
    resultBuffer.push_back(static_cast<uint8_t>(msecValue & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((msecValue >> 8U) & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((msecValue >> 16U) & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((msecValue >> 24U) & 0xFFU));

    return resultBuffer;
}

/**
 * @brief Reads CAN RX/TX signals from shared memory for 1000ms periodicity.
 * @details Iterates all 1000ms rate signals, reads RX and TX data from shared
 *          memory, captures timestamp, validates each signal block, assembles
 *          and returns result vector. Thread-safe: acquires memMutex_ via
 *          std::lock_guard for the duration of the read operation.
 *
 * @return std::vector<uint8_t> Byte vector containing the aggregated CAN
 *         signal data read at 1000ms periodicity. Returns an empty vector
 *         if no valid data is available or an error occurs.
 *
 * @retval empty_vector No valid signal data was available, all reads failed,
 *                      or sharedMemHandle_ is null.
 * @retval non_empty_vector Successfully read and validated CAN signal data
 *                          with appended timestamp bytes.
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
std::vector<uint8_t> CanConsumer::ReadCanData1000ms()
{
    /* Guard: verify shared memory handle is valid */
    if (sharedMemHandle_ == nullptr)
    {
        /* Return empty vector; caller will detect and log */
        std::cerr << "[CanConsumer] ERROR: ReadCanData1000ms called with null shared memory handle."
                  << std::endl;
        return std::vector<uint8_t>null;
    }

    /* Acquire mutex for thread-safe shared memory access */
    const std::lock_guard<std::mutex> lock(memMutex_null);

    /* Capture acquisition timestamp before signal reads */
    const uint64_t timestamp = CollectTimestamp();
    static_cast<void>(timestamp); /* Timestamp stored in members; packed value available if needed */

    /* Initialize output accumulation buffer */
    std::vector<uint8_t> resultBuffernull;

    /* Iterate over all registered signal IDs in the 1000ms group */
    for (const auto& entry : signalIdMap_null)
    {
        const uint32_t signalId = entry.first;

        /* Read RX signal data for this signal ID */
        const std::vector<uint8_t> rxData = ReadCanRxData(signalId);

        /* Read TX signal data for this signal ID */
        const std::vector<uint8_t> txData = ReadCanTxData(signalId);

        /* Validate and append RX data */
        if (ValidateSignalData(rxData) == true)
        {
            /* Append valid RX data to result buffer */
            resultBuffer.insert(resultBuffer.end(), rxData.begin(), rxData.end());
        }
        else
        {
            /* Skip invalid RX entry; log signal ID and continue */
            std::cerr << "[CanConsumer] WARNING: ReadCanData1000ms - Invalid RX data for signalId="
                      << signalId << std::endl;
        }

        /* Validate and append TX data */
        if (ValidateSignalData(txData) == true)
        {
            /* Append valid TX data to result buffer */
            resultBuffer.insert(resultBuffer.end(), txData.begin(), txData.end());
        }
        else
        {
            /* Skip invalid TX entry; log signal ID and continue */
            std::cerr << "[CanConsumer] WARNING: ReadCanData1000ms - Invalid TX data for signalId="
                      << signalId << std::endl;
        }
    }

    /* Append timestamp components to result buffer */
    /* Append timestampSec_ as 4 bytes (little-endian) */
    const uint32_t secValue = timestampSec_;
    resultBuffer.push_back(static_cast<uint8_t>(secValue & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((secValue >> 8U) & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((secValue >> 16U) & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((secValue >> 24U) & 0xFFU));

    /* Append timestampMsec_ as 4 bytes (little-endian) */
    const uint32_t msecValue = timestampMsec_;
    resultBuffer.push_back(static_cast<uint8_t>(msecValue & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((msecValue >> 8U) & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((msecValue >> 16U) & 0xFFU));
    resultBuffer.push_back(static_cast<uint8_t>((msecValue >> 24U) & 0xFFU));

    return resultBuffer;
}

/**
 * @brief Reads a specific CAN RX signal from shared memory by signal ID.
 * @details Accesses the shared memory region using the provided signal ID to
 *          locate and read the corresponding CAN RX signal data via signalIdMap_
 *          offset lookup. Copies raw signal bytes into a local vector. The caller
 *          must hold memMutex_ or ensure exclusive access before invoking this
 *          function. Returns empty vector on any access error.
 *
 * @param[in] signalId Unique identifier of the CAN RX signal to read.
 *                     Valid range: [1, UINT32_MAX]. Zero is treated as invalid.
 *                     Must correspond to a valid signal entry in signalIdMap_.
 *
 * @return std::vector<uint8_t> Byte vector containing the raw CAN RX signal
 *         data. Returns an empty vector if the signal ID is invalid or the
 *         read operation fails.
 *
 * @retval empty_vector Signal ID is zero, not found in signalIdMap_, or
 *                      sharedMemHandle_ is null.
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
std::vector<uint8_t> CanConsumer::ReadCanRxData(const uint32_t signalId)
{
    /* Guard: verify shared memory handle is valid */
    if (sharedMemHandle_ == nullptr)
    {
        /* Return empty vector; caller ValidateSignalData will detect */
        return std::vector<uint8_t>null;
    }

    /* Guard: verify signalId is non-zero */
    if (signalId == 0U)
    {
        std::cerr << "[CanConsumer] WARNING: ReadCanRxData called with zero signalId."
                  << std::endl;
        return std::vector<uint8_t>null;
    }

    /* Compute memory offset from signalId using internal lookup */
    const auto it = signalIdMap_null.find(signalId);
    if (it == signalIdMap_null.end())
    {
        /* Signal ID not found in the map; return empty vector */
        std::cerr << "[CanConsumer] WARNING: ReadCanRxData - signalId="
                  << signalId << " not found in signalIdMap." << std::endl;
        return std::vector<uint8_t>null;
    }

    const uint32_t signalOffset = it->second;

    /* Compute base address of signal data in shared memory */
    const uint8_t* const signalBasePtr =
        static_cast<const uint8_t*>(sharedMemHandle_) + signalOffset;

    /* Determine signal data size from shared memory descriptor (bounded) */
    const uint32_t signalDataSize = kDefaultSignalDataSize;

    /* Validate that the data size is within acceptable bounds */
    if (signalDataSize > kMaxSignalDataSize)
    {
        std::cerr << "[CanConsumer] ERROR: ReadCanRxData - Signal data size exceeds maximum for signalId="
                  << signalId << std::endl;
        return std::vector<uint8_t>null;
    }

    /* Copy signal bytes into result vector */
    std::vector<uint8_t> resultData(signalDataSize, 0U);
    static_cast<void>(std::memcpy(resultData.data(), signalBasePtr, signalDataSize));

    return resultData;
}

/**
 * @brief Reads a specific CAN TX signal from shared memory by signal ID.
 * @details Accesses the shared memory region using the provided signal ID to
 *          locate and read the corresponding CAN TX signal data via signalIdMap_
 *          offset lookup. Copies raw signal bytes into a local vector. The caller
 *          must hold memMutex_ or ensure exclusive access before invoking this
 *          function. Returns empty vector on any access error.
 *
 * @param[in] signalId Unique identifier of the CAN TX signal to read.
 *                     Valid range: [1, UINT32_MAX]. Zero is treated as invalid.
 *                     Must correspond to a valid signal entry in signalIdMap_.
 *
 * @return std::vector<uint8_t> Byte vector containing the raw CAN TX signal
 *         data. Returns an empty vector if the signal ID is invalid or the
 *         read operation fails.
 *
 * @retval empty_vector Signal ID is zero, not found in signalIdMap_, or
 *                      sharedMemHandle_ is null.
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
std::vector<uint8_t> CanConsumer::ReadCanTxData(const uint32_t signalId)
{
    /* Guard: verify shared memory handle is valid */
    if (sharedMemHandle_ == nullptr)
    {
        /* Return empty vector; caller ValidateSignalData will detect */
        return std::vector<uint8_t>null;
    }

    /* Guard: verify signalId is non-zero */
    if (signalId == 0U)
    {
        std::cerr << "[CanConsumer] WARNING: ReadCanTxData called with zero signalId."
                  << std::endl;
        return std::vector<uint8_t>null;
    }

    /* Compute TX signal memory offset from signalId using internal lookup */
    const auto it = signalIdMap_null.find(signalId);
    if (it == signalIdMap_null.end())
    {
        /* Signal ID not found in the map; return empty vector */
        std::cerr << "[CanConsumer] WARNING: ReadCanTxData - signalId="
                  << signalId << " not found in signalIdMap." << std::endl;
        return std::vector<uint8_t>null;
    }

    const uint32_t signalOffset = it->second;

    /* Compute base address of TX signal data in shared memory */
    const uint8_t* const signalBasePtr =
        static_cast<const uint8_t*>(sharedMemHandle_) + signalOffset;

    /* Determine TX signal data size from shared memory descriptor (bounded) */
    const uint32_t signalDataSize = kDefaultSignalDataSize;

    /* Validate that the data size is within acceptable bounds */
    if (signalDataSize > kMaxSignalDataSize)
    {
        std::cerr << "[CanConsumer] ERROR: ReadCanTxData - Signal data size exceeds maximum for signalId="
                  << signalId << std::endl;
        return std::vector<uint8_t>null;
    }

    /* Copy TX signal bytes into result vector */
    std::vector<uint8_t> resultData(signalDataSize, 0U);
    static_cast<void>(std::memcpy(resultData.data(), signalBasePtr, signalDataSize));

    return resultData;
}

/**
 * @brief Captures seconds and milliseconds timestamp at the time of signal collection.
 * @details Reads the current system time using std::chrono::steady_clock,
 *          decomposes it into seconds and milliseconds components, stores them
 *          in timestampSec_ and timestampMsec_ respectively, and returns the
 *          packed 64-bit combination with seconds in the upper 32 bits and
 *          milliseconds in the lower 32 bits. Uses steady_clock for monotonic,
 *          deterministic timing suitable for signal acquisition correlation.
 *
 * @return uint64_t Combined timestamp value. Upper 32 bits: seconds since epoch.
 *         Lower 32 bits: milliseconds within the current second.
 *         Range: [0, UINT64_MAX].
 *
 * @retval 0 Timestamp collection failed or clock source returned zero for both
 *           seconds and milliseconds components.
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
uint64_t CanConsumer::CollectTimestamp()
{
    /* Obtain current system time from platform time service (steady_clock) */
    const auto now = std::chrono::steady_clock::now();
    const auto durationSinceEpoch = now.time_since_epoch();

    /* Convert to milliseconds total */
    const auto totalMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(durationSinceEpoch).count();

    /* Decompose into seconds and milliseconds components */
    const uint32_t currentTimeSec = static_cast<uint32_t>(
        static_cast<uint64_t>(totalMilliseconds) / static_cast<uint64_t>(kMillisecondsPerSecond));
    const uint32_t currentTimeMsec = static_cast<uint32_t>(
        static_cast<uint64_t>(totalMilliseconds) % static_cast<uint64_t>(kMillisecondsPerSecond));

    /* Update private timestamp members for downstream assembly use */
    timestampSec_ = currentTimeSec;
    timestampMsec_ = currentTimeMsec;

    /* Validate timestamp values are within expected range */
    if ((currentTimeSec == 0U) && (currentTimeMsec == 0U))
    {
        /* Log potential time source failure; return zero timestamp as fallback */
        /* Caller will still function but timestamp accuracy may be degraded */
        std::cerr << "[CanConsumer] WARNING: CollectTimestamp - Both seconds and milliseconds "
                  << "are zero. Possible time source failure." << std::endl;
    }

    /* Pack seconds (upper 32 bits) and milliseconds (lower 32 bits) */
    const uint64_t packedTimestamp =
        (static_cast<uint64_t>(currentTimeSec) << kTimestampShiftBits) |
        static_cast<uint64_t>(currentTimeMsec);

    return packedTimestamp;
}

/**
 * @brief Checks if the collected signal data is valid.
 * @details Evaluates the signal data vector for emptiness and size bounds.
 *          Returns true only if the vector is non-empty and does not exceed
 *          the maximum allowed CAN signal data size. This function is purely
 *          a predicate evaluator with no side effects and no state modification.
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
bool CanConsumer::ValidateSignalData(const std::vector<uint8_t>& data) const noexcept
{
    /* Check 1: data vector must not be empty */
    if (data.size() == 0U)
    {
        /* Empty vector indicates read failure */
        return false;
    }

    /* Check 2: data vector must not exceed maximum CAN signal data size */
    const uint32_t maxSignalDataSize = kMaxSignalDataSize;
    if (data.size() > static_cast<size_t>(maxSignalDataSize))
    {
        /* Oversized data indicates shared memory corruption or descriptor error */
        return false;
    }

    /* All validation checks passed */
    return true;
}

} /* namespace probe */