#pragma once

/**
 * @file    CanConsumer.hpp
 * @brief   Declares the CanConsumer class for reading CAN RX/TX signals from shared memory.
 * @details CanConsumer provides periodic and on-request access to CAN bus signals stored
 *          in a shared memory region. It supports 100 ms and 1000 ms cyclic read operations,
 *          individual signal retrieval by ID, timestamp collection, and signal data validation.
 *          Designed for single-threaded periodic task execution within the probe application.
 * @author  Engineering Team
 * @date    2025-01-30
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <string>

/**
 * @defgroup CanConsumerModule CAN Consumer Module
 * @brief Components responsible for consuming CAN RX/TX signal data from shared memory.
 * @details This module provides cyclic and on-request CAN signal reading capabilities,
 *          timestamp acquisition, and signal data validation for the probe application.
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

/**
 * @class   CanConsumer
 * @brief   Reads CAN RX/TX signals from a shared memory region.
 * @details CanConsumer encapsulates all logic required to access CAN bus signal data
 *          stored in a shared memory handle provided at construction time. It exposes
 *          cyclic read methods for 100 ms and 1000 ms periodicities, on-request signal
 *          reads by signal ID, timestamp capture, and signal data validation.
 *
 *          The class is designed for single-threaded use, called from a periodic task
 *          context. All inputs are validated before use. No dynamic memory allocation
 *          is performed beyond std::vector construction for return values.
 *
 * @ingroup CanConsumerModule
 *
 * @note    Compliant with ISO 26262 ASIL-B, MISRA C++:2008, and AUTOSAR C++14 guidelines.
 *          No recursion, no dynamic memory (beyond STL containers), no undefined behaviour.
 *          All fixed-width integer types are used where bit-width matters.
 *
 * @warning The shared memory handle passed at construction must remain valid for the
 *          entire lifetime of this object. Passing a null or dangling pointer results
 *          in undefined behaviour. The caller is responsible for lifetime management.
 *
 * @invariant memHandle_ is non-null after successful construction and is never modified.
 * @invariant timestampSec_ and timestampMsec_ reflect the most recent CollectTimestamp() call.
 * @invariant signalIdMap_ is populated at construction and read-only thereafter.
 *
 * @see ProbeApp
 */
class CanConsumer
{
public:

    // =========================================================================
    // Constants
    // =========================================================================

    /// @brief Maximum number of bytes expected in a single CAN signal payload.
    /// @details CAN 2.0B frames carry at most 8 data bytes. Extended CAN FD frames
    ///          may carry up to 64 bytes; this constant is set conservatively for
    ///          classical CAN compatibility.
    static constexpr std::size_t kMaxSignalPayloadBytes{8U};

    /// @brief Sentinel value returned as an empty vector when a read operation fails.
    /// @details An empty vector (size == 0) is used as the canonical error indicator
    ///          for all ReadCan* methods. Callers must check vector::empty() before use.
    static constexpr std::size_t kEmptyPayloadSize{0U};

    /// @brief Milliseconds per second, used in timestamp composition.
    /// @details Derived from SI definition: 1 s = 1000 ms.
    static constexpr uint64_t kMsPerSecond{1000ULL};

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    /**
     * @brief   Constructs a CanConsumer with the provided shared memory handle.
     * @details Initialises all member variables. The supplied @p memHandle is stored
     *          as a non-owning observer pointer; ownership and lifetime management
     *          remain with the caller (ProbeApp). The signal ID map is populated
     *          during construction to allow O(1) look-ups at runtime.
     *          No heap allocation is performed beyond STL container initialisation.
     *
     * @param[in] memHandle  Non-owning raw pointer to the shared memory region that
     *                       contains CAN RX/TX signal data. Must not be nullptr.
     *                       Valid range: any non-null, properly aligned pointer to a
     *                       shared memory block whose layout is known to this component.
     *
     * @pre  @p memHandle is non-null and points to a valid, accessible shared memory
     *       region whose lifetime exceeds that of this CanConsumer instance.
     * @post memHandle_ == @p memHandle; all other members are zero-initialised;
     *       signalIdMap_ is populated with the default signal catalogue.
     *
     * @throws std::invalid_argument if @p memHandle is nullptr.
     *
     * @note Called by: ProbeApp at application startup.
     * @note Deviation: raw non-owning pointer is used here because the shared memory
     *       handle is an opaque OS resource not suitable for std::unique_ptr ownership
     *       at this interface boundary. Lifetime is guaranteed by the caller.
     *
     * @requirements SWR-REQ-01-01-001
     * @rationale Shared memory is provided externally; the constructor stores only an
     *            observer pointer to avoid double-ownership and to remain compatible
     *            with OS-level shared memory APIs that return void*.
     * @see ProbeApp
     */
    explicit CanConsumer(void* memHandle);

    /**
     * @brief   Destroys the CanConsumer instance.
     * @details Releases no OS resources directly; the shared memory handle is not
     *          owned by this class. All STL members are destroyed automatically via RAII.
     *
     * @post All member variables are destroyed. The shared memory region is unaffected.
     * @throws None — declared noexcept.
     * @note  Default destructor behaviour is sufficient; declared explicitly for
     *        documentation clarity and to satisfy MISRA C++ Rule 12-1-1.
     */
    ~CanConsumer() noexcept = default;

    /// @brief Deleted copy constructor — CanConsumer is non-copyable.
    /// @details Copying would duplicate the non-owning pointer without clear ownership
    ///          semantics, which is prohibited under MISRA M1.
    CanConsumer(const CanConsumer&) = delete;

    /// @brief Deleted copy-assignment operator — CanConsumer is non-copyable.
    CanConsumer& operator=(const CanConsumer&) = delete;

    /// @brief Deleted move constructor — CanConsumer is non-movable.
    /// @details Move semantics are suppressed to prevent accidental transfer of the
    ///          shared memory observer pointer outside its intended scope.
    CanConsumer(CanConsumer&&) = delete;

    /// @brief Deleted move-assignment operator — CanConsumer is non-movable.
    CanConsumer& operator=(CanConsumer&&) = delete;

    // =========================================================================
    // Public Methods
    // =========================================================================

    /**
     * @brief   Reads all CAN RX/TX signals from shared memory for the 100 ms periodicity.
     * @details Iterates over all signal IDs registered in signalIdMap_ whose periodicity
     *          is 100 ms, reads each signal's raw byte payload from the shared memory
     *          region pointed to by memHandle_, and appends the bytes to the result vector.
     *          CollectTimestamp() is called internally to record the acquisition time.
     *          ValidateSignalData() is called on the assembled payload before return.
     *          Returns an empty vector if the shared memory read fails or validation fails.
     *
     * @return  std::vector<uint8_t> containing the concatenated raw byte payloads of all
     *          100 ms CAN signals. Each signal's bytes are appended in ascending signal-ID
     *          order. An empty vector indicates a read or validation failure.
     *
     * @retval  Non-empty vector  All 100 ms signals were read and validated successfully.
     * @retval  Empty vector      Shared memory read failed, memHandle_ is null, or
     *                            ValidateSignalData() returned false.
     *
     * @pre  memHandle_ is non-null and the shared memory region is accessible.
     * @post timestampSec_ and timestampMsec_ reflect the time of this read cycle.
     *
     * @throws None — all errors are communicated via the return value.
     *
     * @note Called by: ProbeApp at 100 ms cyclic rate.
     * @warning Do not call from an interrupt context; this function accesses shared memory
     *          and performs STL container operations.
     *
     * @requirements SWR-REQ-01-001; SWR-REQ-01-01-001
     * @rationale Cyclic reads are separated by periodicity to allow the caller to schedule
     *            them independently and to minimise unnecessary shared memory accesses.
     * @see ReadCanData1000ms, ReadCanRxData, ReadCanTxData
     */
    std::vector<uint8_t> ReadCanData100ms();

    /**
     * @brief   Reads all CAN RX/TX signals from shared memory for the 1000 ms periodicity.
     * @details Iterates over all signal IDs registered in signalIdMap_ whose periodicity
     *          is 1000 ms, reads each signal's raw byte payload from the shared memory
     *          region pointed to by memHandle_, and appends the bytes to the result vector.
     *          CollectTimestamp() is called internally to record the acquisition time.
     *          ValidateSignalData() is called on the assembled payload before return.
     *          Returns an empty vector if the shared memory read fails or validation fails.
     *
     * @return  std::vector<uint8_t> containing the concatenated raw byte payloads of all
     *          1000 ms CAN signals. Each signal's bytes are appended in ascending signal-ID
     *          order. An empty vector indicates a read or validation failure.
     *
     * @retval  Non-empty vector  All 1000 ms signals were read and validated successfully.
     * @retval  Empty vector      Shared memory read failed, memHandle_ is null, or
     *                            ValidateSignalData() returned false.
     *
     * @pre  memHandle_ is non-null and the shared memory region is accessible.
     * @post timestampSec_ and timestampMsec_ reflect the time of this read cycle.
     *
     * @throws None — all errors are communicated via the return value.
     *
     * @note Called by: ProbeApp at 1000 ms cyclic rate.
     * @warning Do not call from an interrupt context; this function accesses shared memory
     *          and performs STL container operations.
     *
     * @requirements SWR-REQ-01-002; SWR-REQ-01-01-001
     * @rationale Separated from ReadCanData100ms() to allow independent scheduling and
     *            to avoid reading low-frequency signals at the 100 ms rate unnecessarily.
     * @see ReadCanData100ms, ReadCanRxData, ReadCanTxData
     */
    std::vector<uint8_t> ReadCanData1000ms();

    /**
     * @brief   Reads a specific CAN RX signal from shared memory by signal ID.
     * @details Looks up @p signalId in signalIdMap_ to obtain the shared memory offset
     *          and byte length for the requested RX signal. Reads the corresponding bytes
     *          from the memory region pointed to by memHandle_ and returns them as a vector.
     *          Returns an empty vector if the signal ID is not found, if memHandle_ is null,
     *          or if the read would exceed the mapped memory bounds.
     *
     * @param[in] signalId  Unique identifier of the CAN RX signal to read.
     *                      Valid range: any uint32_t value present in signalIdMap_.
     *                      Values not present in signalIdMap_ cause an empty-vector return.
     *
     * @return  std::vector<uint8_t> containing the raw byte payload of the requested
     *          RX signal. Size is determined by the signal's registered length in
     *          signalIdMap_. An empty vector indicates failure.
     *
     * @retval  Non-empty vector  Signal found and read successfully.
     * @retval  Empty vector      signalId not in signalIdMap_, memHandle_ is null,
     *                            or memory bounds check failed.
     *
     * @pre  memHandle_ is non-null.
     * @pre  @p signalId is a valid RX signal identifier registered in signalIdMap_.
     * @post Return vector contains the raw bytes of the requested signal, or is empty.
     *
     * @throws None — all errors are communicated via the return value.
     *
     * @note Called by: CanConsumer (internally from ReadCanData100ms / ReadCanData1000ms).
     * @warning Caller must check whether the returned vector is empty before accessing
     *          its contents to avoid undefined behaviour.
     *
     * @requirements SWR-REQ-01-01-001
     * @rationale On-request granular reads allow higher-level logic to retrieve individual
     *            signals without triggering a full cyclic read.
     * @see ReadCanTxData, ReadCanData100ms, ReadCanData1000ms
     */
    std::vector<uint8_t> ReadCanRxData(uint32_t signalId);

    /**
     * @brief   Reads a specific CAN TX signal from shared memory by signal ID.
     * @details Looks up @p signalId in signalIdMap_ to obtain the shared memory offset
     *          and byte length for the requested TX signal. Reads the corresponding bytes
     *          from the memory region pointed to by memHandle_ and returns them as a vector.
     *          Returns an empty vector if the signal ID is not found, if memHandle_ is null,
     *          or if the read would exceed the mapped memory bounds.
     *
     * @param[in] signalId  Unique identifier of the CAN TX signal to read.
     *                      Valid range: any uint32_t value present in signalIdMap_.
     *                      Values not present in signalIdMap_ cause an empty-vector return.
     *
     * @return  std::vector<uint8_t> containing the raw byte payload of the requested
     *          TX signal. Size is determined by the signal's registered length in
     *          signalIdMap_. An empty vector indicates failure.
     *
     * @retval  Non-empty vector  Signal found and read successfully.
     * @retval  Empty vector      signalId not in signalIdMap_, memHandle_ is null,
     *                            or memory bounds check failed.
     *
     * @pre  memHandle_ is non-null.
     * @pre  @p signalId is a valid TX signal identifier registered in signalIdMap_.
     * @post Return vector contains the raw bytes of the requested signal, or is empty.
     *
     * @throws None — all errors are communicated via the return value.
     *
     * @note Called by: CanConsumer (internally from ReadCanData100ms / ReadCanData1000ms).
     * @warning Caller must check whether the returned vector is empty before accessing
     *          its contents to avoid undefined behaviour.
     *
     * @requirements SWR-REQ-01-01-001
     * @rationale Mirrors ReadCanRxData() for TX signals; separation of RX and TX paths
     *            allows independent validation rules and memory layout handling per direction.
     * @see ReadCanRxData, ReadCanData100ms, ReadCanData1000ms
     */
    std::vector<uint8_t> ReadCanTxData(uint32_t signalId);

    /**
     * @brief   Captures the current wall-clock timestamp at the time of signal collection.
     * @details Reads the system monotonic clock and decomposes the result into whole seconds
     *          and millisecond remainder. The values are stored in timestampSec_ and
     *          timestampMsec_ for subsequent use by the caller or by cyclic read methods.
     *          The combined 64-bit return value encodes seconds in the upper 32 bits and
     *          milliseconds in the lower 32 bits:
     *              returnValue = (timestampSec_ * kMsPerSecond) + timestampMsec_
     *          This encoding provides millisecond resolution over a range exceeding
     *          584 million years, which is sufficient for all automotive use cases.
     *
     * @return  uint64_t encoded timestamp: (seconds * 1000) + milliseconds.
     *          Units: milliseconds since epoch (or monotonic clock origin).
     *          Range: [0, UINT64_MAX].
     *
     * @retval  Non-zero value  Timestamp captured successfully.
     * @retval  0               Clock read returned zero (implementation-defined edge case;
     *                          treat as valid unless the system clock is known to be faulty).
     *
     * @pre  System clock is operational and accessible.
     * @post timestampSec_ and timestampMsec_ are updated to reflect the captured time.
     *
     * @throws None — all errors are communicated via the return value.
     *
     * @note Called by: CanConsumer internally before returning signal data.
     * @note Uses std::chrono::steady_clock for monotonic, non-adjustable time source
     *       as required for deterministic automotive timing.
     *
     * @requirements SWR-REQ-01-003; SWR-REQ-03-01-005
     * @rationale A combined uint64_t return avoids the need for an output-parameter struct
     *            while still providing sub-second resolution required by diagnostic logging.
     * @see ReadCanData100ms, ReadCanData1000ms
     */
    uint64_t CollectTimestamp() noexcept;

    /**
     * @brief   Checks whether the supplied signal data vector is valid.
     * @details Performs the following checks in order:
     *            1. The vector is non-empty (size > 0).
     *            2. The vector size does not exceed kMaxSignalPayloadBytes.
     *            3. No byte in the vector equals the reserved error sentinel value (0xFFU).
     *          All three conditions must be satisfied for the function to return true.
     *          This function does not modify any member state.
     *
     * @param[in] data  Byte vector containing the raw CAN signal payload to validate.
     *                  Valid range: 1 to kMaxSignalPayloadBytes bytes; no 0xFF sentinel bytes.
     *
     * @return  bool indicating whether @p data passes all validation checks.
     *
     * @retval  true   @p data is non-empty, within size bounds, and contains no error sentinels.
     * @retval  false  @p data is empty, exceeds kMaxSignalPayloadBytes, or contains 0xFF.
     *
     * @pre  None — the function is safe to call with any vector, including empty ones.
     * @post No member state is modified.
     *
     * @throws None — declared noexcept.
     *
     * @note Called by: CanConsumer internally after each read operation.
     * @note The 0xFF sentinel check is based on the shared memory initialisation pattern
     *       used by the platform; a fully set byte indicates an uninitialised slot.
     *
     * @requirements SWR-REQ-01-01-001
     * @rationale Centralising validation in a single function ensures consistent error
     *            detection across all read paths and simplifies unit testing.
     * @see ReadCanRxData, ReadCanTxData, ReadCanData100ms, ReadCanData1000ms
     */
    bool ValidateSignalData(const std::vector<uint8_t>& data) const noexcept;

private:

    // =========================================================================
    // Private Types
    // =========================================================================

    /**
     * @struct  SignalDescriptor
     * @brief   Describes the location and size of a single CAN signal in shared memory.
     * @details Holds the byte offset from the base of the shared memory region and the
     *          number of bytes occupied by the signal payload. Used as the mapped value
     *          in signalIdMap_.
     * @ingroup CanConsumerModule
     * @note    All fields are zero-initialised at construction.
     */
    struct SignalDescriptor
    {
        std::size_t offset{0U};  ///< @brief Byte offset from shared memory base. Range: [0, region size).
        std::size_t length{0U};  ///< @brief Number of payload bytes for this signal. Range: [1, kMaxSignalPayloadBytes].
    };

    // =========================================================================
    // Private Methods
    // =========================================================================

    /**
     * @brief   Reads raw bytes from the shared memory region at a given offset.
     * @details Performs a bounds-checked byte copy from the memory region pointed to by
     *          memHandle_. Returns an empty vector if @p offset + @p length would exceed
     *          the mapped region, or if memHandle_ is null.
     *          This is the single point of shared memory access for all read operations,
     *          ensuring that bounds checking is applied uniformly.
     *
     * @param[in] offset  Byte offset from the start of the shared memory region.
     *                    Valid range: [0, mapped region size - length].
     * @param[in] length  Number of bytes to read. Valid range: [1, kMaxSignalPayloadBytes].
     *
     * @return  std::vector<uint8_t> containing @p length bytes starting at @p offset,
     *          or an empty vector on bounds violation or null handle.
     *
     * @retval  Non-empty vector  Bytes read successfully.
     * @retval  Empty vector      Null handle, zero length, or out-of-bounds access.
     *
     * @pre  memHandle_ is non-null (checked internally; returns empty on violation).
     * @pre  @p length > 0.
     * @post Shared memory region is not modified.
     *
     * @throws None — all errors are communicated via the return value.
     *
     * @note This method is the sole accessor of memHandle_; all other read methods
     *       delegate to it to enforce the single-responsibility principle (M7).
     * @warning reinterpret_cast is used to convert void* to const uint8_t*. This is
     *          the only permitted use of reinterpret_cast in this class (MISRA M10 deviation).
     *          Deviation documented here: the shared memory API provides a void* handle;
     *          byte-level access requires reinterpret_cast to uint8_t*.
     *
     * @rationale Centralising raw memory access in one private method limits the blast
     *            radius of any pointer arithmetic error and simplifies code review.
     */
    std::vector<uint8_t> ReadBytesFromSharedMemory(std::size_t offset, std::size_t length) const;

    /**
     * @brief   Populates signalIdMap_ with the default CAN signal catalogue.
     * @details Called once from the constructor. Inserts all known RX and TX signal
     *          descriptors (offset and length) keyed by their uint32_t signal IDs.
     *          The catalogue is derived from the CAN database (DBC) associated with
     *          this software component.
     *
     * @pre  signalIdMap_ is empty.
     * @post signalIdMap_ contains all registered signal descriptors.
     *
     * @throws std::bad_alloc if the unordered_map cannot allocate memory (propagated).
     *
     * @note Called only from the constructor; not intended for external use.
     * @rationale Separating catalogue initialisation into its own method keeps the
     *            constructor body concise and makes the catalogue easy to update
     *            independently (SRP, M7).
     */
    void InitialiseSignalMap();

    // =========================================================================
    // Private Member Variables
    // =========================================================================

    void* memHandle_{nullptr};
    ///< @brief Non-owning observer pointer to the shared memory region containing CAN signal data.
    ///         Must remain non-null and valid for the lifetime of this object.
    ///         Set once at construction; never modified thereafter.

    uint32_t timestampSec_{0U};
    ///< @brief Whole-seconds component of the most recently captured timestamp.
    ///         Units: seconds. Range: [0, UINT32_MAX].
    ///         Updated by every call to CollectTimestamp().

    uint32_t timestampMsec_{0U};
    ///< @brief Millisecond-remainder component of the most recently captured timestamp.
    ///         Units: milliseconds. Range: [0, 999].
    ///         Updated by every call to CollectTimestamp().

    std::unordered_map<uint32_t, SignalDescriptor> signalIdMap_null;
    ///< @brief Maps each registered CAN signal ID to its shared memory descriptor (offset + length).
    ///         Populated once during construction via InitialiseSignalMap().
    ///         Read-only after construction; no thread-safety mechanism required for reads
    ///         in a single-threaded periodic task context.

    std::size_t sharedMemRegionSize_{0U};
    ///< @brief Total size in bytes of the shared memory region pointed to by memHandle_.
    ///         Used for bounds checking in ReadBytesFromSharedMemory().
    ///         Units: bytes. Range: [1, SIZE_MAX]. Set at construction.

}; // class CanConsumer

} // namespace probe