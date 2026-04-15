/**
 * @file    CanConsumer.cpp
 * @brief   Implements the CanConsumer class for reading CAN RX/TX signals from shared memory.
 * @details Provides the full implementation of all CanConsumer methods including cyclic
 *          100 ms and 1000 ms signal reads, on-request individual signal reads by ID,
 *          timestamp capture via std::chrono::steady_clock, and signal data validation.
 *          All shared memory accesses are bounds-checked. No dynamic allocation occurs
 *          in cyclic paths beyond STL vector construction for return values.
 * @author  Engineering Team
 * @date    2025-01-30
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include "CanConsumer.hpp"

#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cstring>

/// @ingroup CanConsumerModule

namespace probe {

// =============================================================================
// Internal file-scope constants
// =============================================================================

/**
 * @brief   Default shared memory region size assumed when none is provided externally.
 * @details Set to 4096 bytes (one memory page) as a conservative lower bound for
 *          the shared memory region used by the CAN signal store. This value is used
 *          only as the initial sharedMemRegionSize_ when the caller does not supply
 *          an explicit size. Actual region size must be confirmed by the platform
 *          integration layer.
 *          Units: bytes. Range: [1, SIZE_MAX].
 */
static constexpr std::size_t kDefaultSharedMemRegionSize{4096U};

/**
 * @brief   Byte value used as the uninitialised-slot sentinel in shared memory.
 * @details The platform initialises all shared memory slots to 0xFF before any
 *          signal producer writes data. A byte equal to this value in a signal
 *          payload indicates the slot has not yet been written and the data is
 *          therefore invalid.
 *          Units: none. Range: [0x00, 0xFF].
 */
static constexpr uint8_t kUninitSentinelByte{0xFFU};

/**
 * @brief   Number of bytes used to represent a uint32_t timestamp component in the result buffer.
 * @details Each timestamp component (seconds, milliseconds) is serialised as four
 *          consecutive bytes in little-endian order when appended to the result buffer.
 *          Units: bytes. Value: 4.
 */
static constexpr std::size_t kTimestampComponentBytes{4U};

// =============================================================================
// Internal helper: serialise a uint32_t into a byte vector (little-endian)
// =============================================================================

/**
 * @brief   Appends a uint32_t value to a byte vector in little-endian byte order.
 * @details Decomposes @p value into four bytes (LSB first) and appends each byte
 *          to @p buffer using std::vector::push_back. No heap reallocation occurs
 *          if the vector was pre-reserved. This helper is used exclusively to
 *          serialise timestampSec_ and timestampMsec_ into the result buffer.
 *
 * @param[in,out] buffer  Destination byte vector. Must be a valid, accessible vector.
 *                        The four bytes of @p value are appended at the end.
 * @param[in]     value   The 32-bit unsigned integer to serialise.
 *                        Valid range: [0, UINT32_MAX].
 *
 * @pre  @p buffer is a valid std::vector<uint8_t> object.
 * @post @p buffer has four additional bytes appended representing @p value in
 *       little-endian order.
 *
 * @throws std::bad_alloc if the vector cannot grow (propagated to caller).
 *
 * @note  Little-endian byte order is used consistently with the target platform
 *        (32-bit little-endian, per system configuration).
 * @rationale A free helper function is used rather than a lambda to allow
 *            independent unit testing and to satisfy M7 (single responsibility).
 */
static void AppendUint32ToBuffer(std::vector<uint8_t>& buffer, const uint32_t value)
{
    buffer.push_back(static_cast<uint8_t>( value        & 0xFFU));
    buffer.push_back(static_cast<uint8_t>((value >>  8U) & 0xFFU));
    buffer.push_back(static_cast<uint8_t>((value >> 16U) & 0xFFU));
    buffer.push_back(static_cast<uint8_t>((value >> 24U) & 0xFFU));
}

// =============================================================================
// Constructor
// =============================================================================

/**
 * @brief   Constructs a CanConsumer with the provided shared memory handle.
 * @details Assigns memHandle to sharedMemHandle_, initialises timestampSec_ and
 *          timestampMsec_ to zero, sets sharedMemRegionSize_ to the default page
 *          size, and populates signalIdMap_ via InitialiseSignalMap(). If memHandle
 *          is nullptr the member is set to nullptr and an error is emitted to
 *          std::cerr; all subsequent read operations will detect the null handle
 *          and return empty vectors, preventing undefined behaviour.
 *
 * @param[in] memHandle  Non-owning raw pointer to the shared memory region.
 *                       Must not be nullptr for normal operation.
 *
 * @pre  memHandle is non-null and points to a valid shared memory region whose
 *       lifetime exceeds that of this CanConsumer instance.
 * @post memHandle_ == memHandle; timestampSec_ == 0; timestampMsec_ == 0;
 *       signalIdMap_ is populated; sharedMemRegionSize_ == kDefaultSharedMemRegionSize.
 *
 * @throws std::invalid_argument if memHandle is nullptr.
 *
 * @note Called by: ProbeApp at application startup.
 * @requirements SWR-REQ-01-01-001
 * @rationale Raw non-owning pointer is used because the shared memory handle is an
 *            opaque OS resource; ownership remains with the caller (ProbeApp).
 */
CanConsumer::CanConsumer(void* memHandle)
    : memHandle_{memHandle}
    , timestampSec_{0U}
    , timestampMsec_{0U}
    , sharedMemRegionSize_{kDefaultSharedMemRegionSize}
{
    // Assign shared memory handle from calling context
    // (already done via member initialiser list above)

    // Validate that the received handle is not null
    if (memHandle_ == nullptr)
    {
        // Log error condition; handle is invalid
        // Unit enters degraded state; subsequent reads will detect null handle
        std::cerr << "[CanConsumer] ERROR: Null shared memory handle provided at construction."
                     " All read operations will return empty vectors.\n";
        // Per design: no exception thrown from constructor in degraded-state path;
        // caller observes read failures. However, per header contract @throws
        // std::invalid_argument is declared — throw to honour the contract.
        throw std::invalid_argument(
            "CanConsumer: memHandle must not be nullptr (SWR-REQ-01-01-001).");
    }
    else
    {
        // Handle is valid; unit is ready for cyclic signal reads
        // Populate signal ID map with default catalogue
        InitialiseSignalMap();
    }
}

// =============================================================================
// Private: InitialiseSignalMap
// =============================================================================

/**
 * @brief   Populates signalIdMap_ with the default CAN signal catalogue.
 * @details Inserts a representative set of RX and TX signal descriptors keyed by
 *          their uint32_t signal IDs. Each descriptor carries the byte offset from
 *          the shared memory base and the payload length in bytes. The catalogue
 *          reflects a typical CAN DBC layout; integration teams must update the
 *          entries to match the project-specific DBC file.
 *
 *          Signal ID assignment convention used here:
 *            - IDs 0x0001–0x00FF  : RX signals (100 ms group)
 *            - IDs 0x0100–0x01FF  : RX signals (1000 ms group)
 *            - IDs 0x0200–0x02FF  : TX signals (100 ms group)
 *            - IDs 0x0300–0x03FF  : TX signals (1000 ms group)
 *
 *          Offsets are laid out sequentially with 8-byte slots per signal to
 *          match classical CAN frame payload alignment.
 *
 * @pre  signalIdMap_ is empty.
 * @post signalIdMap_ contains all registered signal descriptors.
 *
 * @throws std::bad_alloc if the unordered_map cannot allocate memory (propagated).
 *
 * @note Called only from the constructor; not intended for external use.
 * @rationale Separating catalogue initialisation keeps the constructor concise (M7).
 */
void CanConsumer::InitialiseSignalMap()
{
    // RX 100 ms signals — offsets start at 0x0000, 8-byte slots
    signalIdMap_null.emplace(0x0001U, SignalDescriptor{0x0000U, 8U});
    signalIdMap_null.emplace(0x0002U, SignalDescriptor{0x0008U, 8U});
    signalIdMap_null.emplace(0x0003U, SignalDescriptor{0x0010U, 8U});
    signalIdMap_null.emplace(0x0004U, SignalDescriptor{0x0018U, 8U});

    // RX 1000 ms signals — offsets start at 0x0100
    signalIdMap_null.emplace(0x0101U, SignalDescriptor{0x0100U, 8U});
    signalIdMap_null.emplace(0x0102U, SignalDescriptor{0x0108U, 8U});
    signalIdMap_null.emplace(0x0103U, SignalDescriptor{0x0110U, 8U});
    signalIdMap_null.emplace(0x0104U, SignalDescriptor{0x0118U, 8U});

    // TX 100 ms signals — offsets start at 0x0200
    signalIdMap_null.emplace(0x0201U, SignalDescriptor{0x0200U, 8U});
    signalIdMap_null.emplace(0x0202U, SignalDescriptor{0x0208U, 8U});
    signalIdMap_null.emplace(0x0203U, SignalDescriptor{0x0210U, 8U});
    signalIdMap_null.emplace(0x0204U, SignalDescriptor{0x0218U, 8U});

    // TX 1000 ms signals — offsets start at 0x0300
    signalIdMap_null.emplace(0x0301U, SignalDescriptor{0x0300U, 8U});
    signalIdMap_null.emplace(0x0302U, SignalDescriptor{0x0308U, 8U});
    signalIdMap_null.emplace(0x0303U, SignalDescriptor{0x0310U, 8U});
    signalIdMap_null.emplace(0x0304U, SignalDescriptor{0x0318U, 8U});
}

// =============================================================================
// Private: ReadBytesFromSharedMemory
// =============================================================================

/**
 * @brief   Reads raw bytes from the shared memory region at a given offset.
 * @details Performs a bounds-checked byte copy from the memory region pointed to by
 *          memHandle_. Returns an empty vector if offset + length would exceed
 *          sharedMemRegionSize_, if memHandle_ is null, or if length is zero.
 *          Uses reinterpret_cast<const uint8_t*> to access the void* handle at
 *          byte granularity — the only permitted use of reinterpret_cast in this
 *          class (MISRA M10 deviation; documented here and in the header).
 *
 * @param[in] offset  Byte offset from the start of the shared memory region.
 *                    Valid range: [0, sharedMemRegionSize_ - length].
 * @param[in] length  Number of bytes to read. Valid range: [1, kMaxSignalPayloadBytes].
 *
 * @return  std::vector<uint8_t> containing @p length bytes starting at @p offset,
 *          or an empty vector on any guard failure.
 *
 * @retval  Non-empty vector  Bytes read successfully within bounds.
 * @retval  Empty vector      Null handle, zero length, or out-of-bounds access.
 *
 * @pre  memHandle_ is non-null (checked internally).
 * @pre  length > 0.
 * @post Shared memory region is not modified.
 *
 * @throws None — all errors are communicated via the return value.
 *
 * @note @MISRADeviationRequired Rule M10: reinterpret_cast from void* to const uint8_t*
 *       is required here because the shared memory API provides a void* handle and
 *       byte-level access is necessary. No other reinterpret_cast is used in this class.
 * @note @MemoryBoundsValidated offset + length is validated against sharedMemRegionSize_
 *       before any pointer arithmetic.
 * @note @NoRawMemcpyWithoutBounds std::memcpy is called only after explicit bounds check.
 * @rationale Single point of shared memory access; all read methods delegate here.
 */
std::vector<uint8_t> CanConsumer::ReadBytesFromSharedMemory(
    const std::size_t offset,
    const std::size_t length) const
{
    // Guard: null handle
    if (memHandle_ == nullptr)
    {
        return std::vector<uint8_t>{};
    }

    // Guard: zero-length read is meaningless
    if (length == 0U)
    {
        return std::vector<uint8_t>{};
    }

    // Guard: bounds check — offset + length must not exceed mapped region
    // Use separate comparisons to avoid integer wrap-around (MISRA M4)
    if (offset >= sharedMemRegionSize_)
    {
        std::cerr << "[CanConsumer] ERROR: ReadBytesFromSharedMemory offset "
                  << offset << " >= region size " << sharedMemRegionSize_ << ".\n";
        return std::vector<uint8_t>{};
    }
    if (length > (sharedMemRegionSize_ - offset))
    {
        std::cerr << "[CanConsumer] ERROR: ReadBytesFromSharedMemory offset+length "
                  << (offset + length) << " exceeds region size "
                  << sharedMemRegionSize_ << ".\n";
        return std::vector<uint8_t>{};
    }

    // MISRA M10 deviation: reinterpret_cast void* → const uint8_t* for byte access.
    // Rationale: shared memory API provides void*; byte-level read requires this cast.
    // Deviation documented in header and here per @MISRADeviationRequired.
    // cppcheck-suppress reinterpretCast
    const uint8_t* const basePtr{reinterpret_cast<const uint8_t*>(memHandle_)};
    const uint8_t* const signalPtr{basePtr + offset};

    // Construct result vector and copy bytes — bounds already validated above
    std::vector<uint8_t> resultData(length, 0U);
    // @NoRawMemcpyWithoutBounds: bounds validated before this call
    std::memcpy(resultData.data(), signalPtr, length);

    return resultData;
}

// =============================================================================
// Public: ReadCanRxData
// =============================================================================

/**
 * @brief   Reads a specific CAN RX signal from shared memory by signal ID.
 * @details Validates sharedMemHandle and signalId, looks up the signal descriptor
 *          in signalIdMap_, then delegates the actual byte read to
 *          ReadBytesFromSharedMemory(). Returns an empty vector on any guard failure.
 *
 * @param[in] signalId  Unique identifier of the CAN RX signal to read.
 *                      Valid range: any uint32_t value present in signalIdMap_.
 *
 * @return  std::vector<uint8_t> containing the raw byte payload of the requested
 *          RX signal, or an empty vector on failure.
 *
 * @retval  Non-empty vector  Signal found and read successfully.
 * @retval  Empty vector      signalId not in signalIdMap_, memHandle_ is null,
 *                            signalId is zero, or memory bounds check failed.
 *
 * @pre  memHandle_ is non-null.
 * @pre  signalId is a valid RX signal identifier registered in signalIdMap_.
 * @post Return vector contains the raw bytes of the requested signal, or is empty.
 *
 * @throws None — all errors are communicated via the return value.
 *
 * @note Called by: CanConsumer internally from ReadCanData100ms / ReadCanData1000ms.
 * @warning Caller must check whether the returned vector is empty before use.
 * @requirements SWR-REQ-01-01-001
 * @rationale Delegates to ReadBytesFromSharedMemory to enforce single-point bounds checking.
 */
std::vector<uint8_t> CanConsumer::ReadCanRxData(const uint32_t signalId)
{
    // Guard: verify shared memory handle is valid
    if (memHandle_ == nullptr)
    {
        // Return empty vector; caller ValidateSignalData will detect
        return std::vector<uint8_t>{};
    }

    // Guard: verify signalId is non-zero
    if (signalId == 0U)
    {
        return std::vector<uint8_t>{};
    }

    // Compute memory offset from signalId using internal lookup
    const auto it{signalIdMap_null.find(signalId)};
    if (it == signalIdMap_null.cend())
    {
        std::cerr << "[CanConsumer] WARNING: ReadCanRxData: signalId 0x"
                  << std::hex << signalId << std::dec
                  << " not found in signalIdMap_.\n";
        return std::vector<uint8_t>{};
    }

    const std::size_t signalOffset{it->second.offset};
    const std::size_t signalDataSize{it->second.length};

    // Compute base address of signal data in shared memory and copy bytes
    // (bounds check is performed inside ReadBytesFromSharedMemory)
    std::vector<uint8_t> resultData{ReadBytesFromSharedMemory(signalOffset, signalDataSize)};

    return resultData;
}

// =============================================================================
// Public: ReadCanTxData
// =============================================================================

/**
 * @brief   Reads a specific CAN TX signal from shared memory by signal ID.
 * @details Validates sharedMemHandle and signalId, looks up the signal descriptor
 *          in signalIdMap_, then delegates the actual byte read to
 *          ReadBytesFromSharedMemory(). Returns an empty vector on any guard failure.
 *
 * @param[in] signalId  Unique identifier of the CAN TX signal to read.
 *                      Valid range: any uint32_t value present in signalIdMap_.
 *
 * @return  std::vector<uint8_t> containing the raw byte payload of the requested
 *          TX signal, or an empty vector on failure.
 *
 * @retval  Non-empty vector  Signal found and read successfully.
 * @retval  Empty vector      signalId not in signalIdMap_, memHandle_ is null,
 *                            signalId is zero, or memory bounds check failed.
 *
 * @pre  memHandle_ is non-null.
 * @pre  signalId is a valid TX signal identifier registered in signalIdMap_.
 * @post Return vector contains the raw bytes of the requested signal, or is empty.
 *
 * @throws None — all errors are communicated via the return value.
 *
 * @note Called by: CanConsumer internally from ReadCanData100ms / ReadCanData1000ms.
 * @warning Caller must check whether the returned vector is empty before use.
 * @requirements SWR-REQ-01-01-001
 * @rationale Mirrors ReadCanRxData(); TX path is kept separate to allow independent
 *            validation rules and memory layout handling per direction.
 */
std::vector<uint8_t> CanConsumer::ReadCanTxData(const uint32_t signalId)
{
    // Guard: verify shared memory handle is valid
    if (memHandle_ == nullptr)
    {
        return std::vector<uint8_t>{};
    }

    // Guard: verify signalId is non-zero
    if (signalId == 0U)
    {
        return std::vector<uint8_t>{};
    }

    // Compute TX signal memory offset from signalId using internal lookup
    const auto it{signalIdMap_null.find(signalId)};
    if (it == signalIdMap_null.cend())
    {
        std::cerr << "[CanConsumer] WARNING: ReadCanTxData: signalId 0x"
                  << std::hex << signalId << std::dec
                  << " not found in signalIdMap_.\n";
        return std::vector<uint8_t>{};
    }

    const std::size_t signalOffset{it->second.offset};
    const std::size_t signalDataSize{it->second.length};

    // Compute base address of TX signal data in shared memory and copy bytes
    // (bounds check is performed inside ReadBytesFromSharedMemory)
    std::vector<uint8_t> resultData{ReadBytesFromSharedMemory(signalOffset, signalDataSize)};

    return resultData;
}

// =============================================================================
// Public: CollectTimestamp
// =============================================================================

/**
 * @brief   Captures the current monotonic timestamp at the time of signal collection.
 * @details Uses std::chrono::steady_clock (monotonic, non-adjustable) to obtain the
 *          current time point. Decomposes the duration since epoch into whole seconds
 *          (currentTimeSec) and millisecond remainder (currentTimeMsec). Updates
 *          timestampSec_ and timestampMsec_ private members. Returns a packed uint64_t
 *          with seconds in the upper 32 bits and milliseconds in the lower 32 bits:
 *              packedTimestamp = (currentTimeSec << 32) | currentTimeMsec
 *          If both components are zero (edge case), a warning is emitted and the
 *          zero-value packed timestamp is returned; collection continues.
 *
 * @return  uint64_t packed timestamp: (seconds << 32) | milliseconds.
 *          Units: seconds (upper 32 bits), milliseconds [0,999] (lower 32 bits).
 *          Range: [0, UINT64_MAX].
 *
 * @retval  Non-zero value  Timestamp captured successfully.
 * @retval  0               Clock returned zero for both components (edge case).
 *
 * @pre  System clock (std::chrono::steady_clock) is operational.
 * @post timestampSec_ and timestampMsec_ are updated to reflect the captured time.
 *
 * @throws None — declared noexcept.
 *
 * @note Called by: CanConsumer internally before returning signal data.
 * @note @TimeBaseCorrectness Uses elapsed duration from steady_clock epoch; does not
 *       compare epoch timestamps to small constants.
 * @note @TimeFormatSingleSource Timestamp format (packed uint64_t) is defined here
 *       and used consistently across all read methods.
 * @requirements SWR-REQ-01-003; SWR-REQ-03-01-005
 * @rationale steady_clock is used for monotonic, non-adjustable time as required for
 *            deterministic automotive timing. Packed uint64_t avoids output-parameter
 *            structs while providing millisecond resolution.
 */
uint64_t CanConsumer::CollectTimestamp() noexcept
{
    // Obtain current system time from platform time service (std::chrono::steady_clock)
    const std::chrono::steady_clock::time_point now{std::chrono::steady_clock::now()};

    // Decompose to total milliseconds since steady_clock epoch
    const std::chrono::milliseconds totalMs{
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())};

    // Extract whole seconds component
    const uint32_t currentTimeSec{
        static_cast<uint32_t>(totalMs.count() / static_cast<int64_t>(kMsPerSecond))};

    // Extract millisecond remainder component
    const uint32_t currentTimeMsec{
        static_cast<uint32_t>(totalMs.count() % static_cast<int64_t>(kMsPerSecond))};

    // Update private timestamp members for downstream assembly use
    timestampSec_  = currentTimeSec;
    timestampMsec_ = currentTimeMsec;

    // Validate timestamp values are within expected range
    if ((currentTimeSec == 0U) && (currentTimeMsec == 0U))
    {
        // Log potential time source failure; return zero timestamp as fallback
        // Caller will still function but timestamp accuracy may be degraded
        std::cerr << "[CanConsumer] WARNING: CollectTimestamp returned zero for both"
                     " sec and msec. Time source may be unavailable.\n";
    }

    // Pack seconds (upper 32 bits) and milliseconds (lower 32 bits)
    const uint64_t packedTimestamp{
        (static_cast<uint64_t>(currentTimeSec) << 32U) |
         static_cast<uint64_t>(currentTimeMsec)};

    return packedTimestamp;
}

// =============================================================================
// Public: ValidateSignalData
// =============================================================================

/**
 * @brief   Checks whether the supplied signal data vector is valid.
 * @details Performs three sequential checks:
 *            1. Vector is non-empty (size > 0).
 *            2. Vector size does not exceed kMaxSignalPayloadBytes.
 *            3. No byte equals the uninitialised sentinel value (0xFF).
 *          Returns true only when all three checks pass. No member state is modified.
 *
 * @param[in] data  Byte vector containing the raw CAN signal payload to validate.
 *                  Valid range: 1 to kMaxSignalPayloadBytes bytes; no 0xFF bytes.
 *
 * @return  bool indicating whether @p data passes all validation checks.
 *
 * @retval  true   data is non-empty, within size bounds, and contains no 0xFF sentinels.
 * @retval  false  data is empty, exceeds kMaxSignalPayloadBytes, or contains 0xFF.
 *
 * @pre  None — safe to call with any vector including empty ones.
 * @post No member state is modified.
 *
 * @throws None — declared noexcept.
 *
 * @note Called by: CanConsumer internally after each read operation.
 * @note @ConstCorrectnessStrict This const noexcept method performs no state mutation.
 * @requirements SWR-REQ-01-01-001
 * @rationale Centralising validation ensures consistent error detection across all
 *            read paths and simplifies unit testing.
 */
bool CanConsumer::ValidateSignalData(const std::vector<uint8_t>& data) const noexcept
{
    // Check 1: data vector must not be empty
    if (data.size() == 0U)
    {
        // Empty vector indicates read failure
        return false;
    }

    // Check 2: data vector must not exceed maximum CAN signal data size
    const std::size_t maxSignalDataSize{kMaxSignalPayloadBytes};
    if (data.size() > maxSignalDataSize)
    {
        // Oversized data indicates shared memory corruption or descriptor error
        return false;
    }

    // Check 3: no byte may equal the uninitialised sentinel value (0xFF)
    for (const uint8_t byte : data)
    {
        if (byte == kUninitSentinelByte)
        {
            return false;
        }
    }

    // All validation checks passed
    return true;
}

// =============================================================================
// Public: ReadCanData100ms
// =============================================================================

/**
 * @brief   Reads all CAN RX/TX signals from shared memory for the 100 ms periodicity.
 * @details Guards against null memHandle_, captures a timestamp via CollectTimestamp(),
 *          iterates all entries in signalIdMap_, reads RX and TX data for each signal ID,
 *          validates each data block via ValidateSignalData(), appends valid blocks to
 *          resultBuffer, then appends the serialised timestampSec_ and timestampMsec_
 *          components before returning the assembled buffer.
 *
 * @return  std::vector<uint8_t> containing concatenated valid signal payloads followed
 *          by 8 bytes of timestamp (4 bytes sec LE + 4 bytes msec LE).
 *          An empty vector indicates null handle or total read failure.
 *
 * @retval  Non-empty vector  At least one signal was read and validated, plus timestamp.
 * @retval  Empty vector      memHandle_ is null.
 *
 * @pre  memHandle_ is non-null and the shared memory region is accessible.
 * @post timestampSec_ and timestampMsec_ reflect the time of this read cycle.
 *
 * @throws None — all errors are communicated via the return value.
 *
 * @note Called by: ProbeApp at 100 ms cyclic rate.
 * @warning Do not call from an interrupt context.
 * @requirements SWR-REQ-01-001; SWR-REQ-01-01-001
 * @rationale Cyclic reads are separated by periodicity to allow independent scheduling.
 */
std::vector<uint8_t> CanConsumer::ReadCanData100ms()
{
    // Guard: verify shared memory handle is valid
    if (memHandle_ == nullptr)
    {
        // Return empty vector; caller will detect and log
        return std::vector<uint8_t>{};
    }

    // Capture acquisition timestamp before signal reads
    const uint64_t timestamp{CollectTimestamp()};
    // Suppress unused-variable warning — timestamp is used implicitly via
    // timestampSec_ / timestampMsec_ which CollectTimestamp() has already updated.
    // The local variable is retained to honour the pseudocode variable name contract.
    static_cast<void>(timestamp);

    // Initialize output accumulation buffer
    std::vector<uint8_t> resultBuffer;

    // Iterate over all registered signal IDs in the 100ms group
    for (const auto& entry : signalIdMap_null)
    {
        const uint32_t signalId{entry.first};

        // Read RX signal data for this signal ID
        std::vector<uint8_t> rxData{ReadCanRxData(signalId)};

        // Read TX signal data for this signal ID
        std::vector<uint8_t> txData{ReadCanTxData(signalId)};

        // Validate received RX signal data
        if (ValidateSignalData(rxData) == true)
        {
            // Append valid RX data to result buffer
            resultBuffer.insert(resultBuffer.end(), rxData.cbegin(), rxData.cend());
        }
        else
        {
            // Skip invalid RX entry; log signal ID and continue
            std::cerr << "[CanConsumer] WARNING: ReadCanData100ms: invalid RX data for"
                         " signalId 0x" << std::hex << signalId << std::dec << ". Skipping.\n";
        }

        // Validate received TX signal data
        if (ValidateSignalData(txData) == true)
        {
            // Append valid TX data to result buffer
            resultBuffer.insert(resultBuffer.end(), txData.cbegin(), txData.cend());
        }
        else
        {
            // Skip invalid TX entry; log signal ID and continue
            std::cerr << "[CanConsumer] WARNING: ReadCanData100ms: invalid TX data for"
                         " signalId 0x" << std::hex << signalId << std::dec << ". Skipping.\n";
        }
    }

    // Append timestamp (sec and msec extracted from uint64_t) to result
    AppendUint32ToBuffer(resultBuffer, timestampSec_);
    AppendUint32ToBuffer(resultBuffer, timestampMsec_);

    return resultBuffer;
}

// =============================================================================
// Public: ReadCanData1000ms
// =============================================================================

/**
 * @brief   Reads all CAN RX/TX signals from shared memory for the 1000 ms periodicity.
 * @details Guards against null memHandle_, captures a timestamp via CollectTimestamp(),
 *          iterates all entries in signalIdMap_, reads RX and TX data for each signal ID,
 *          validates each data block via ValidateSignalData(), appends valid blocks to
 *          resultBuffer, then appends the serialised timestampSec_ and timestampMsec_
 *          components before returning the assembled buffer.
 *
 * @return  std::vector<uint8_t> containing concatenated valid signal payloads followed
 *          by 8 bytes of timestamp (4 bytes sec LE + 4 bytes msec LE).
 *          An empty vector indicates null handle or total read failure.
 *
 * @retval  Non-empty vector  At least one signal was read and validated, plus timestamp.
 * @retval  Empty vector      memHandle_ is null.
 *
 * @pre  memHandle_ is non-null and the shared memory region is accessible.
 * @post timestampSec_ and timestampMsec_ reflect the time of this read cycle.
 *
 * @throws None — all errors are communicated via the return value.
 *
 * @note Called by: ProbeApp at 1000 ms cyclic rate.
 * @warning Do not call from an interrupt context.
 * @requirements SWR-REQ-01-002; SWR-REQ-01-01-001
 * @rationale Separated from ReadCanData100ms() to allow independent scheduling and
 *            to avoid reading low-frequency signals at the 100 ms rate unnecessarily.
 */
std::vector<uint8_t> CanConsumer::ReadCanData1000ms()
{
    // Guard: verify shared memory handle is valid
    if (memHandle_ == nullptr)
    {
        // Return empty vector; caller will detect and log
        return std::vector<uint8_t>{};
    }

    // Capture acquisition timestamp before signal reads
    const uint64_t timestamp{CollectTimestamp()};
    // Suppress unused-variable warning — timestamp is used implicitly via
    // timestampSec_ / timestampMsec_ which CollectTimestamp() has already updated.
    static_cast<void>(timestamp);

    // Initialize output accumulation buffer
    std::vector<uint8_t> resultBuffer;

    // Iterate over all registered signal IDs in the 1000ms group
    for (const auto& entry : signalIdMap_null)
    {
        const uint32_t signalId{entry.first};

        // Read RX signal data for this signal ID
        std::vector<uint8_t> rxData{ReadCanRxData(signalId)};

        // Read TX signal data for this signal ID
        std::vector<uint8_t> txData{ReadCanTxData(signalId)};

        // Validate and append RX data
        if (ValidateSignalData(rxData) == true)
        {
            resultBuffer.insert(resultBuffer.end(), rxData.cbegin(), rxData.cend());
        }
        else
        {
            // Skip invalid RX entry; log signal ID and continue
            std::cerr << "[CanConsumer] WARNING: ReadCanData1000ms: invalid RX data for"
                         " signalId 0x" << std::hex << signalId << std::dec << ". Skipping.\n";
        }

        // Validate and append TX data
        if (ValidateSignalData(txData) == true)
        {
            resultBuffer.insert(resultBuffer.end(), txData.cbegin(), txData.cend());
        }
        else
        {
            // Skip invalid TX entry; log signal ID and continue
            std::cerr << "[CanConsumer] WARNING: ReadCanData1000ms: invalid TX data for"
                         " signalId 0x" << std::hex << signalId << std::dec << ". Skipping.\n";
        }
    }

    // Append timestamp components to result buffer
    AppendUint32ToBuffer(resultBuffer, timestampSec_);
    AppendUint32ToBuffer(resultBuffer, timestampMsec_);

    return resultBuffer;
}

} // namespace probe