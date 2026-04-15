#pragma once

/**
 * @file    ProbeCommVariant.hpp
 * @brief   Manages variant configuration and feature flag resolution for the probe communication system.
 * @details Reads a 5-byte variant code from shared memory during startup and maps it against a
 *          variant dictionary to enable or disable individual probe data collection features
 *          (regular probe, event probe, GEDR, DAQ transmission). Provides safe fallback behaviour
 *          when the variant code is absent or unrecognised, disabling all data transmission to
 *          prevent unintended system operation. Designed for single-threaded periodic-task execution
 *          in an ISO 26262 ASIL-B bare-metal environment.
 * @author  Engineering Team
 * @date    2025-01-30
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include <cstdint>
#include <string>
#include <unordered_map>

/**
 * @defgroup ProbeCommVariantGroup Probe Communication Variant Management
 * @brief Components responsible for variant-code-driven feature flag resolution.
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

/**
 * @class   ProbeCommVariant
 * @brief   Variant configuration manager that resolves feature flags from a shared-memory variant code.
 * @details Reads a 5-byte variant code from shared memory at startup via the SharedMemory_VariantCode
 *          interface. The code is looked up in an internal variant dictionary; matching entries
 *          activate or deactivate individual data-collection features (regular probe, event probe,
 *          event probe without picture, GEDR, GEDR without picture, DAQ transmission). When the
 *          variant code is not found in the dictionary, HandleMissingVariantCode() is invoked and
 *          all transmission is disabled via DisableAllTransmissionOnInvalidVariant() to ensure
 *          fail-safe behaviour compliant with SWR-REQ-01-03-003 and SWR-REQ-03-14-003.
 *
 *          Lifecycle:
 *          1. Constructed by ProbeApp at startup.
 *          2. ProbeApp calls ReadVariantCode() to populate variantCode_.
 *          3. ProbeApp calls SetVariant() with the returned code to apply feature flags.
 *          4. Feature-check methods are called on-request throughout the application lifetime.
 *          5. Destroyed by ProbeApp on shutdown.
 *
 * @ingroup ProbeCommVariantGroup
 * @note    Complies with MISRA C++:2008, ISO 26262 ASIL-B, and C++14/17 coding standards.
 *          No dynamic memory allocation is performed after construction.
 *          All public query methods are const and noexcept.
 * @warning Must not be instantiated more than once per process. Caller is responsible for
 *          ensuring ReadVariantCode() and SetVariant() are called exactly once at startup
 *          before any feature-check methods are invoked.
 * @invariant variantCode_.size() == 0U || variantCode_.size() == kVariantCodeLength
 *            All boolean feature flags are initialised to false and only set to true by SetVariant().
 * @see     SharedMem
 */
class ProbeCommVariant
{
public:

    // =========================================================================
    // Constants
    // =========================================================================

    /**
     * @brief   Expected byte-length of a valid variant code string.
     * @details Derived from the system requirement that variant codes are exactly 5 ASCII
     *          characters. Used for validation in ReadVariantCode() and SetVariant().
     */
    static constexpr std::size_t kVariantCodeLength{5U};

    /**
     * @brief   Default variant code applied when shared memory read fails.
     * @details An empty string signals an invalid/missing variant, causing
     *          DisableAllTransmissionOnInvalidVariant() to be invoked.
     */
    static constexpr const char* kDefaultVariantCode{""};

    /**
     * @brief   File-system path to the variant dictionary configuration file.
     * @details Relative path used at runtime to locate the variant-to-feature-flag mapping.
     *          Must be accessible from the working directory of the probe application.
     */
    static constexpr const char* kVariantDictionaryPath{"config/variant_dictionary.cfg"};

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    /**
     * @brief   Constructs the variant configuration manager with safe default values.
     * @details Initialises all feature-flag members to false, sets variantCode_ and
     *          defaultVariantCode_ to empty strings, and sets variantReadSuccess_ to false.
     *          No shared-memory access is performed in the constructor; that is deferred to
     *          ReadVariantCode() to allow controlled startup sequencing by ProbeApp.
     * @pre     None — default-constructible with no external dependencies.
     * @post    All feature flags are false. variantCode_ == "". variantReadSuccess_ == false.
     * @throws  None — constructor is noexcept.
     * @note    Called by: ProbeApp.
     * @requirements SWR-REQ-01-03-001;SWR-REQ-03-14-001
     * @rationale Deferred initialisation pattern ensures the constructor cannot fail,
     *            simplifying error handling at the call site.
     */
    ProbeCommVariant() noexcept;

    /**
     * @brief   Destroys the variant configuration manager and releases all resources.
     * @details Performs an orderly teardown. No heap memory is allocated by this class,
     *          so destruction is trivial. Declared virtual to support potential future
     *          subclassing while remaining safe under RAII.
     * @pre     Object must have been fully constructed.
     * @post    All resources held by this instance are released.
     * @throws  None — destructor is noexcept.
     * @note    Called by: ProbeApp.
     */
    ~ProbeCommVariant() noexcept;

    // Prevent copy and move to enforce single-instance ownership semantics.

    /// @brief Deleted copy constructor — ProbeCommVariant is non-copyable.
    ProbeCommVariant(const ProbeCommVariant&) = delete;

    /// @brief Deleted copy-assignment operator — ProbeCommVariant is non-copyable.
    ProbeCommVariant& operator=(const ProbeCommVariant&) = delete;

    /// @brief Deleted move constructor — ProbeCommVariant is non-movable.
    ProbeCommVariant(ProbeCommVariant&&) = delete;

    /// @brief Deleted move-assignment operator — ProbeCommVariant is non-movable.
    ProbeCommVariant& operator=(ProbeCommVariant&&) = delete;

    // =========================================================================
    // Public Interface — SharedMemory_VariantCode (Provided Interface)
    // =========================================================================

    /**
     * @brief   Reads the 5-byte variant code from shared memory during initialisation.
     * @details Implements the SharedMemory_VariantCode provided interface. Accesses the
     *          shared memory segment managed by the SharedMem component to retrieve the
     *          variant code written by the boot software. If the read succeeds and the
     *          returned string has exactly kVariantCodeLength characters, variantReadSuccess_
     *          is set to true and the code is stored in variantCode_. If the read fails or
     *          the length is invalid, variantCode_ is set to kDefaultVariantCode (empty string)
     *          and variantReadSuccess_ is set to false, triggering fallback behaviour in
     *          SetVariant(). This method must be called exactly once at startup before
     *          SetVariant() is invoked.
     * @return  std::string containing the raw variant code read from shared memory,
     *          or an empty string on failure.
     * @retval  Non-empty 5-character string  Variant code successfully read from shared memory.
     * @retval  Empty string                  Read failed or data length invalid; fallback applied.
     * @pre     Shared memory segment must be initialised and accessible.
     * @post    variantCode_ holds the read value or empty string. variantReadSuccess_ reflects outcome.
     * @throws  None — all errors are handled internally and reflected in variantReadSuccess_.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @warning Must be called before SetVariant(). Calling feature-check methods before
     *          SetVariant() will return false for all flags.
     * @see     SetVariant()
     * @requirements SWR-REQ-01-03-001;SWR-REQ-03-14-001
     * @rationale Returns std::string to decouple the caller from raw buffer management
     *            and to allow straightforward dictionary lookup in SetVariant().
     */
    std::string ReadVariantCode();

    /**
     * @brief   Applies feature flags by matching the supplied variant code against the variant dictionary.
     * @details Looks up the provided code string in the internal variant dictionary
     *          (variantDictionary_). If a match is found, the corresponding boolean feature
     *          flags (regularProbeEnabled_, eventProbeEnabled_, eventProbeWithoutPictureEnabled_,
     *          gedrEnabled_, gedrWithoutPictureEnabled_, daqTransmissionEnabled_) are set
     *          according to the dictionary entry. If no match is found, HandleMissingVariantCode()
     *          is called, which in turn calls DisableAllTransmissionOnInvalidVariant() to set all
     *          flags to false. The method validates that code is not empty and has the correct
     *          length before performing the lookup.
     * @param[in] code  The variant code string to look up. Must be exactly kVariantCodeLength
     *                  characters. An empty or incorrectly sized string is treated as invalid.
     * @pre     ReadVariantCode() must have been called. code must be a valid std::string.
     * @post    Feature flags reflect the variant dictionary entry for code, or all flags are
     *          false if code was not found or was invalid.
     * @throws  None — all error paths are handled internally.
     * @note    Called by: ProbeApp. Call condition: Startup.
     * @warning Calling this method more than once will overwrite previously set feature flags.
     * @see     ReadVariantCode(), HandleMissingVariantCode(), DisableAllTransmissionOnInvalidVariant()
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     * @rationale Separating read and set operations allows ProbeApp to inspect the raw code
     *            before committing to feature-flag application.
     */
    void SetVariant(const std::string& code);

    // =========================================================================
    // Public Interface — Feature Flag Queries
    // =========================================================================

    /**
     * @brief   Returns whether regular probe data collection is enabled for the active variant.
     * @details Provides a const, noexcept accessor for the regularProbeEnabled_ flag that was
     *          set by SetVariant(). Returns false if SetVariant() has not yet been called or if
     *          the variant code was not found in the dictionary.
     * @return  bool indicating the enabled state of regular probe data collection.
     * @retval  true   Regular probe data collection is enabled for the active variant.
     * @retval  false  Regular probe data collection is disabled, or variant was not resolved.
     * @pre     SetVariant() should have been called at startup for a meaningful result.
     * @post    No state is modified.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: On_Request.
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     */
    [[nodiscard]] bool CheckRegularProbeEnabled() const noexcept;

    /**
     * @brief   Returns whether event probe data collection is enabled for the active variant.
     * @details Provides a const, noexcept accessor for the eventProbeEnabled_ flag that was
     *          set by SetVariant(). Returns false if SetVariant() has not yet been called or if
     *          the variant code was not found in the dictionary.
     * @return  bool indicating the enabled state of event probe data collection.
     * @retval  true   Event probe data collection is enabled for the active variant.
     * @retval  false  Event probe data collection is disabled, or variant was not resolved.
     * @pre     SetVariant() should have been called at startup for a meaningful result.
     * @post    No state is modified.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: On_Request.
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     */
    [[nodiscard]] bool CheckEventProbeEnabled() const noexcept;

    /**
     * @brief   Returns whether event probe without picture is enabled for the active variant.
     * @details Provides a const, noexcept accessor for the eventProbeWithoutPictureEnabled_ flag
     *          that was set by SetVariant(). Returns false if SetVariant() has not yet been called
     *          or if the variant code was not found in the dictionary.
     * @return  bool indicating the enabled state of event probe without picture.
     * @retval  true   Event probe without picture is enabled for the active variant.
     * @retval  false  Event probe without picture is disabled, or variant was not resolved.
     * @pre     SetVariant() should have been called at startup for a meaningful result.
     * @post    No state is modified.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: On_Request.
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     */
    [[nodiscard]] bool CheckEventProbeWithoutPictureEnabled() const noexcept;

    /**
     * @brief   Returns whether GEDR data collection is enabled for the active variant.
     * @details Provides a const, noexcept accessor for the gedrEnabled_ flag that was set by
     *          SetVariant(). Returns false if SetVariant() has not yet been called or if the
     *          variant code was not found in the dictionary.
     * @return  bool indicating the enabled state of GEDR data collection.
     * @retval  true   GEDR data collection is enabled for the active variant.
     * @retval  false  GEDR data collection is disabled, or variant was not resolved.
     * @pre     SetVariant() should have been called at startup for a meaningful result.
     * @post    No state is modified.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: On_Request.
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     */
    [[nodiscard]] bool CheckGedrEnabled() const noexcept;

    /**
     * @brief   Returns whether GEDR without picture is enabled for the active variant.
     * @details Provides a const, noexcept accessor for the gedrWithoutPictureEnabled_ flag that
     *          was set by SetVariant(). Returns false if SetVariant() has not yet been called or
     *          if the variant code was not found in the dictionary.
     * @return  bool indicating the enabled state of GEDR without picture.
     * @retval  true   GEDR without picture is enabled for the active variant.
     * @retval  false  GEDR without picture is disabled, or variant was not resolved.
     * @pre     SetVariant() should have been called at startup for a meaningful result.
     * @post    No state is modified.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: On_Request.
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     */
    [[nodiscard]] bool CheckGedrWithoutPictureEnabled() const noexcept;

    /**
     * @brief   Returns whether DAQ transmission is enabled for the active variant.
     * @details Provides a const, noexcept accessor for the daqTransmissionEnabled_ flag that was
     *          set by SetVariant(). Per SWR-REQ-01-03-003, this flag is explicitly set to false
     *          when the variant code is not found in the dictionary, preventing any unintended
     *          data transmission to the DAQ system.
     * @return  bool indicating the enabled state of DAQ transmission.
     * @retval  true   DAQ transmission is enabled for the active variant.
     * @retval  false  DAQ transmission is disabled; variant code was not found or is invalid.
     * @pre     SetVariant() should have been called at startup for a meaningful result.
     * @post    No state is modified.
     * @throws  None.
     * @note    Called by: ProbeApp. Call condition: On_Request.
     * @requirements SWR-REQ-01-03-002;SWR-REQ-01-03-003;SWR-REQ-03-14-002
     */
    [[nodiscard]] bool CheckDaqTransmissionEnabled() const noexcept;

private:

    // =========================================================================
    // Private Helper Methods
    // =========================================================================

    /**
     * @brief   Logs an error condition and prevents data transmission when the variant code is absent.
     * @details Called internally by SetVariant() when the supplied variant code is not found in
     *          the variant dictionary. Emits a diagnostic message to std::cerr identifying the
     *          unrecognised code, then delegates to DisableAllTransmissionOnInvalidVariant() to
     *          enforce the fail-safe state. This two-step design separates diagnostic reporting
     *          from state mutation, satisfying the single-responsibility principle (M7).
     * @pre     variantCode_ must contain the code that failed lookup.
     * @post    DisableAllTransmissionOnInvalidVariant() has been called; all feature flags are false.
     * @throws  None.
     * @note    Called by: ProbeCommVariant (internal). Call condition: Startup.
     * @warning Must only be called from SetVariant(). Do not call directly from external code.
     * @see     DisableAllTransmissionOnInvalidVariant(), SetVariant()
     * @requirements SWR-REQ-01-03-003;SWR-REQ-03-14-003
     * @rationale Isolating the error-logging step allows unit tests to verify diagnostic output
     *            independently of the flag-reset logic.
     */
    void HandleMissingVariantCode();

    /**
     * @brief   Disables all data-sending feature flags when the variant is invalid or unrecognised.
     * @details Sets regularProbeEnabled_, eventProbeEnabled_, eventProbeWithoutPictureEnabled_,
     *          gedrEnabled_, gedrWithoutPictureEnabled_, and daqTransmissionEnabled_ all to false.
     *          This is the canonical fail-safe state required by SWR-REQ-01-03-003: no data is
     *          transmitted to DAQ or GEDR when the variant cannot be resolved. Called exclusively
     *          from HandleMissingVariantCode().
     * @pre     None — safe to call in any state.
     * @post    All six feature-flag members are false.
     * @throws  None.
     * @note    Called by: ProbeCommVariant (internal). Call condition: Startup.
     * @warning This method unconditionally clears all flags. Any previously set flags will be lost.
     * @see     HandleMissingVariantCode()
     * @requirements SWR-REQ-01-03-003;SWR-REQ-03-14-003
     * @rationale Centralising the flag-reset logic in a dedicated method ensures that no flag
     *            is accidentally omitted when the invalid-variant path is taken.
     */
    void DisableAllTransmissionOnInvalidVariant() noexcept;

    // =========================================================================
    // Private Types
    // =========================================================================

    /**
     * @struct  VariantFeatureFlags
     * @brief   Aggregates all feature-enable flags for a single variant dictionary entry.
     * @details Used as the mapped value type in variantDictionary_ to associate a variant code
     *          string with its complete set of feature flags in a single lookup operation.
     * @ingroup ProbeCommVariantGroup
     * @note    Plain aggregate — no invariants beyond member initialisation.
     */
    struct VariantFeatureFlags
    {
        bool regularProbeEnabled{false};              ///< @brief Enable regular probe data collection.
        bool eventProbeEnabled{false};                ///< @brief Enable event probe data collection.
        bool eventProbeWithoutPictureEnabled{false};  ///< @brief Enable event probe without picture.
        bool gedrEnabled{false};                      ///< @brief Enable GEDR data collection.
        bool gedrWithoutPictureEnabled{false};        ///< @brief Enable GEDR without picture.
        bool daqTransmissionEnabled{false};           ///< @brief Enable DAQ transmission.
    };

    // =========================================================================
    // Private Member Variables
    // =========================================================================

    std::string variantCode_null;
    ///< @brief Active variant code resolved from shared memory. Length == kVariantCodeLength when valid,
    ///<        or empty string when read failed or code was not found. Range: 5-char ASCII or "".

    std::string defaultVariantCode_null;
    ///< @brief Fallback variant code used when shared memory read fails.
    ///<        Initialised to kDefaultVariantCode (empty string). Read-only after construction.

    bool variantReadSuccess_{false};
    ///< @brief Indicates whether the last ReadVariantCode() call successfully retrieved a
    ///<        well-formed 5-byte code from shared memory. true == success, false == failure/fallback.

    bool regularProbeEnabled_{false};
    ///< @brief Feature flag: regular probe data collection enabled for the active variant.
    ///<        Set by SetVariant(); cleared by DisableAllTransmissionOnInvalidVariant().

    bool eventProbeEnabled_{false};
    ///< @brief Feature flag: event probe data collection enabled for the active variant.
    ///<        Set by SetVariant(); cleared by DisableAllTransmissionOnInvalidVariant().

    bool eventProbeWithoutPictureEnabled_{false};
    ///< @brief Feature flag: event probe without picture enabled for the active variant.
    ///<        Set by SetVariant(); cleared by DisableAllTransmissionOnInvalidVariant().

    bool gedrEnabled_{false};
    ///< @brief Feature flag: GEDR data collection enabled for the active variant.
    ///<        Set by SetVariant(); cleared by DisableAllTransmissionOnInvalidVariant().

    bool gedrWithoutPictureEnabled_{false};
    ///< @brief Feature flag: GEDR without picture enabled for the active variant.
    ///<        Set by SetVariant(); cleared by DisableAllTransmissionOnInvalidVariant().

    bool daqTransmissionEnabled_{false};
    ///< @brief Feature flag: DAQ transmission enabled for the active variant.
    ///<        Explicitly false when variant code is not found (SWR-REQ-01-03-003).
    ///<        Set by SetVariant(); cleared by DisableAllTransmissionOnInvalidVariant().

    std::string variantDictionaryPath_null;
    ///< @brief File-system path to the variant dictionary configuration file.
    ///<        Initialised to kVariantDictionaryPath. Used by SetVariant() to load the mapping.

    std::unordered_map<std::string, VariantFeatureFlags> variantDictionary_null;
    ///< @brief In-memory variant dictionary mapping 5-character variant code strings to their
    ///<        corresponding VariantFeatureFlags entries. Populated during construction from
    ///<        the configuration file at variantDictionaryPath_. Empty map == no valid variants.

}; // class ProbeCommVariant

} // namespace probe