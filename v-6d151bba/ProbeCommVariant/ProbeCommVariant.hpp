#pragma once
/**
 * @file   ProbeCommVariant.hpp
 * @brief  Variant configuration manager for probe communication features.
 * @details Manages the reading and interpretation of variant codes from shared memory,
 *          and controls feature enablement flags for regular probe, event probe, GEDR,
 *          and DAQ transmission based on a variant dictionary lookup.
 *          Provides the SharedMemory_VariantCode interface to the system.
 * @author  Engineering Team
 * @date    2024-01-01
 * @version 1.0.0
 * @copyright Copyright (c) 2024 Company. All rights reserved.
 */

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <optional>

#include "../SharedMem/SharedMem.hpp"

/**
 * @defgroup ProbeCommVariantModule Probe Communication Variant Module
 * @brief Components responsible for variant-based feature configuration of probe communication.
 */

/// @namespace probe
/// @brief Contains all components of the probe application.
namespace probe {

/**
 * @class ProbeCommVariant
 * @brief Manages variant code reading and feature flag configuration for probe communication.
 * @details This class is responsible for reading a 5-byte variant code from shared memory
 *          during system startup, matching it against a variant dictionary, and enabling
 *          or disabling collection features (regular probe, event probe, GEDR, DAQ
 *          transmission) accordingly. When the variant code is not found in the dictionary,
 *          all data transmission is disabled as a safety measure.
 *          This class provides the SharedMemory_VariantCode interface.
 *          Thread-safety: All public methods are safe to call from a single thread context.
 *          Designed for single-threaded invocation from ProbeApp.
 * @ingroup ProbeCommVariantModule
 * @note Compliant with MISRA C++ guidelines, ISO 26262 ASIL-B, and CERT C/C++ secure coding.
 *       All variables are initialized at declaration. No dynamic memory allocation via raw new/delete.
 * @warning Feature flags default to disabled (false). The variant code must be read and set
 *          before querying any feature enablement status. Failure to do so will result in
 *          all features reporting as disabled.
 * @invariant All boolean feature flags remain consistent with the last call to SetVariant()
 *            or DisableAllTransmissionOnInvalidVariant(). The variantCode_ member always
 *            contains either the code read from shared memory or the default variant code.
 * @see SharedMem
 */
class ProbeCommVariant {
public:
    /**
     * @brief Constructs the variant configuration manager with default values.
     * @details Initializes all member variables to their default safe states.
     *          All feature flags are initialized to false (disabled). The variant code
     *          is set to the default variant code. This constructor does not perform
     *          any shared memory access or dictionary lookup.
     * @throws None — This constructor is noexcept.
     * @pre None.
     * @post All feature flags are false. variantCode_ equals defaultVariantCode_.
     *       variantReadSuccess_ is false.
     * @note Called by ProbeApp during component initialization.
     * @warning Must be followed by ReadVariantCode() and SetVariant() before querying features.
     * @see ReadVariantCode, SetVariant
     * @requirements SWR-REQ-01-03-001;SWR-REQ-03-14-001
     * @rationale Initializes ProbeCommVariant with safe defaults to ensure deterministic
     *            behavior even if subsequent initialization steps fail.
     */
    ProbeCommVariant() noexcept;

    /**
     * @brief Cleans up variant configuration manager resources.
     * @details Releases any resources held by the variant configuration manager.
     *          Since this class uses only stack-allocated and RAII-managed members,
     *          destruction is straightforward with no special cleanup required.
     * @throws None — This destructor is noexcept.
     * @pre Object is in a valid state.
     * @post Object is destroyed and all resources are released.
     * @note Called by ProbeApp during component shutdown.
     * @see ProbeCommVariant
     */
    ~ProbeCommVariant() noexcept;

    /**
     * @brief Reads the 5-byte variant code from shared memory during initialization.
     * @details Accesses shared memory via the SharedMem component to retrieve the
     *          variant code. If the read operation fails or the data is invalid,
     *          falls back to the default variant code. Sets the variantReadSuccess_
     *          flag to indicate whether the read was successful.
     *          This method provides the SharedMemory_VariantCode interface.
     * @return The variant code string read from shared memory, or the default
     *         variant code if the read operation failed.
     * @retval non-empty string Successfully read variant code from shared memory
     *         or default variant code on fallback.
     * @throws None — Errors are handled internally with fallback to default.
     * @pre ProbeCommVariant object has been constructed.
     * @post variantCode_ is updated with the read value or default.
     *       variantReadSuccess_ reflects the outcome of the read operation.
     * @note Called by ProbeApp at startup. This method should be called exactly once
     *       during the initialization sequence.
     * @warning Shared memory must be available and properly initialized before calling
     *          this method. If shared memory is unavailable, the default variant code
     *          is used silently.
     * @see SetVariant, SharedMem
     * @requirements SWR-REQ-01-03-001;SWR-REQ-03-14-001
     * @rationale Reads variant code from shared memory to determine which probe
     *            features should be active for the current vehicle configuration.
     */
    std::string ReadVariantCode();

    /**
     * @brief Enables or disables collection features based on variant code matched against variant dictionary.
     * @details Looks up the provided variant code in the variant dictionary. If found,
     *          sets the individual feature flags (regular probe, event probe, event probe
     *          without picture, GEDR, GEDR without picture, DAQ transmission) according
     *          to the dictionary entry. If the code is not found, calls
     *          HandleMissingVariantCode() to disable all transmission and log the error.
     * @param[in] code The variant code string to look up in the variant dictionary.
     *                  Expected length: 5 characters. Valid range: any non-empty string
     *                  that may exist in the variant dictionary.
     * @throws None — Errors are handled internally via HandleMissingVariantCode().
     * @pre ReadVariantCode() has been called and returned a valid code.
     *      The variant dictionary is available at variantDictionaryPath_.
     * @post All feature flags are updated according to the dictionary entry for the
     *       given code, or all flags are set to false if the code is not found.
     * @note Called by ProbeApp at startup after ReadVariantCode().
     * @warning If the variant code is not found in the dictionary, all data transmission
     *          will be disabled as a safety measure per SWR-REQ-01-03-003.
     * @see ReadVariantCode, HandleMissingVariantCode, DisableAllTransmissionOnInvalidVariant
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     * @rationale Sets variant and applies feature flags to control which probe data
     *            collection and transmission features are active for the current variant.
     */
    void SetVariant(const std::string& code);

    /**
     * @brief Returns whether regular probe data collection is enabled based on variant.
     * @details Checks the regularProbeEnabled_ flag which was set during SetVariant().
     *          This is a simple accessor with no side effects.
     * @return True if regular probe data collection is enabled, false otherwise.
     * @retval true  Regular probe data collection is enabled for the current variant.
     * @retval false Regular probe data collection is disabled, or variant has not been set.
     * @throws None — This function is noexcept.
     * @pre SetVariant() has been called with a valid variant code.
     * @post No state change. This is a const query method.
     * @note Called by ProbeApp on request to determine if regular probe data should be collected.
     * @see SetVariant, CheckEventProbeEnabled
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     * @rationale Checks if regular probe is enabled to allow ProbeApp to conditionally
     *            activate regular probe data collection.
     */
    bool CheckRegularProbeEnabled() const noexcept;

    /**
     * @brief Returns whether event probe data collection is enabled based on variant.
     * @details Checks the eventProbeEnabled_ flag which was set during SetVariant().
     *          This is a simple accessor with no side effects.
     * @return True if event probe data collection is enabled, false otherwise.
     * @retval true  Event probe data collection is enabled for the current variant.
     * @retval false Event probe data collection is disabled, or variant has not been set.
     * @throws None — This function is noexcept.
     * @pre SetVariant() has been called with a valid variant code.
     * @post No state change. This is a const query method.
     * @note Called by ProbeApp on request to determine if event probe data should be collected.
     * @see SetVariant, CheckRegularProbeEnabled, CheckEventProbeWithoutPictureEnabled
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     * @rationale Checks if event probe is enabled to allow ProbeApp to conditionally
     *            activate event probe data collection.
     */
    bool CheckEventProbeEnabled() const noexcept;

    /**
     * @brief Returns whether event probe without picture is enabled based on variant.
     * @details Checks the eventProbeWithoutPictureEnabled_ flag which was set during
     *          SetVariant(). This is a simple accessor with no side effects.
     * @return True if event probe without picture is enabled, false otherwise.
     * @retval true  Event probe without picture is enabled for the current variant.
     * @retval false Event probe without picture is disabled, or variant has not been set.
     * @throws None — This function is noexcept.
     * @pre SetVariant() has been called with a valid variant code.
     * @post No state change. This is a const query method.
     * @note Called by ProbeApp on request.
     * @see SetVariant, CheckEventProbeEnabled
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     * @rationale Checks if event probe without picture is enabled to allow ProbeApp
     *            to conditionally activate this specific collection mode.
     */
    bool CheckEventProbeWithoutPictureEnabled() const noexcept;

    /**
     * @brief Returns whether GEDR data collection is enabled based on variant.
     * @details Checks the gedrEnabled_ flag which was set during SetVariant().
     *          This is a simple accessor with no side effects.
     * @return True if GEDR data collection is enabled, false otherwise.
     * @retval true  GEDR data collection is enabled for the current variant.
     * @retval false GEDR data collection is disabled, or variant has not been set.
     * @throws None — This function is noexcept.
     * @pre SetVariant() has been called with a valid variant code.
     * @post No state change. This is a const query method.
     * @note Called by ProbeApp on request to determine if GEDR data should be collected.
     * @see SetVariant, CheckGedrWithoutPictureEnabled
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     * @rationale Checks if GEDR is enabled to allow ProbeApp to conditionally
     *            activate GEDR data collection.
     */
    bool CheckGedrEnabled() const noexcept;

    /**
     * @brief Returns whether GEDR without picture is enabled based on variant.
     * @details Checks the gedrWithoutPictureEnabled_ flag which was set during
     *          SetVariant(). This is a simple accessor with no side effects.
     * @return True if GEDR without picture is enabled, false otherwise.
     * @retval true  GEDR without picture is enabled for the current variant.
     * @retval false GEDR without picture is disabled, or variant has not been set.
     * @throws None — This function is noexcept.
     * @pre SetVariant() has been called with a valid variant code.
     * @post No state change. This is a const query method.
     * @note Called by ProbeApp on request.
     * @see SetVariant, CheckGedrEnabled
     * @requirements SWR-REQ-01-03-002;SWR-REQ-03-14-002
     * @rationale Checks if GEDR without picture is enabled to allow ProbeApp
     *            to conditionally activate this specific collection mode.
     */
    bool CheckGedrWithoutPictureEnabled() const noexcept;

    /**
     * @brief Returns whether DAQ transmission is enabled; disabled when variant code not found.
     * @details Checks the daqTransmissionEnabled_ flag which was set during SetVariant().
     *          This flag is explicitly set to false when the variant code is not found
     *          in the dictionary, preventing any data transmission to DAQ.
     *          This is a simple accessor with no side effects.
     * @return True if DAQ transmission is enabled, false otherwise.
     * @retval true  DAQ transmission is enabled for the current variant.
     * @retval false DAQ transmission is disabled, variant not found, or variant has not been set.
     * @throws None — This function is noexcept.
     * @pre SetVariant() has been called with a valid variant code.
     * @post No state change. This is a const query method.
     * @note Called by ProbeApp on request to determine if data should be transmitted to DAQ.
     * @warning Returns false by default if variant code was not found in dictionary,
     *          ensuring no data is transmitted for unknown variants.
     * @see SetVariant, HandleMissingVariantCode, DisableAllTransmissionOnInvalidVariant
     * @requirements SWR-REQ-01-03-002;SWR-REQ-01-03-003;SWR-REQ-03-14-002
     * @rationale Checks if DAQ transmission is enabled to prevent data transmission
     *            when the variant is unknown or invalid.
     */
    bool CheckDaqTransmissionEnabled() const noexcept;

private:
    /**
     * @brief Logs error and prevents data transmission when variant code not found in dictionary.
     * @details Called internally by SetVariant() when the provided variant code does not
     *          match any entry in the variant dictionary. Logs an error message to std::cerr
     *          and invokes DisableAllTransmissionOnInvalidVariant() to set all feature flags
     *          to false, ensuring no data is transmitted for an unknown variant.
     * @throws None — Errors are handled internally.
     * @pre SetVariant() has been called with a code not present in the variant dictionary.
     * @post All feature flags are set to false. An error message has been logged.
     * @note Called internally by ProbeCommVariant (SetVariant).
     * @warning This method disables ALL transmission features. Recovery requires a new
     *          call to SetVariant() with a valid variant code.
     * @see SetVariant, DisableAllTransmissionOnInvalidVariant
     * @requirements SWR-REQ-01-03-003;SWR-REQ-03-14-003
     * @rationale Handles missing variant code as a safety measure to prevent data
     *            transmission with an unrecognized vehicle configuration.
     */
    void HandleMissingVariantCode();

    /**
     * @brief Disables all data sending to DAQ and GEDR when variant is invalid.
     * @details Sets all feature enablement flags (regularProbeEnabled_, eventProbeEnabled_,
     *          eventProbeWithoutPictureEnabled_, gedrEnabled_, gedrWithoutPictureEnabled_,
     *          daqTransmissionEnabled_) to false. This is the safety fallback mechanism
     *          that ensures no data is transmitted when the variant configuration is unknown.
     * @throws None — This function is noexcept.
     * @pre Called from HandleMissingVariantCode() or SetVariant() when variant is invalid.
     * @post All six feature flags are set to false.
     * @note Called internally by ProbeCommVariant.
     * @warning After this call, no probe data collection or transmission will occur
     *          until SetVariant() is called again with a valid variant code.
     * @see HandleMissingVariantCode, SetVariant
     * @requirements SWR-REQ-01-03-003;SWR-REQ-03-14-003
     * @rationale Disables all transmission on invalid variant as a safety measure
     *            compliant with ISO 26262 ASIL-B requirements.
     */
    void DisableAllTransmissionOnInvalidVariant() noexcept;

    /// @brief Current variant code read from shared memory. Default: defaultVariantCode_.
    std::string variantCode_null;

    /// @brief Default variant code used as fallback when shared memory read fails. Length: 5 characters.
    const std::string defaultVariantCode_{"00000"};

    /// @brief Flag indicating whether regular probe data collection is enabled. Default: false.
    bool regularProbeEnabled_{false};

    /// @brief Flag indicating whether event probe data collection is enabled. Default: false.
    bool eventProbeEnabled_{false};

    /// @brief Flag indicating whether event probe without picture is enabled. Default: false.
    bool eventProbeWithoutPictureEnabled_{false};

    /// @brief Flag indicating whether GEDR data collection is enabled. Default: false.
    bool gedrEnabled_{false};

    /// @brief Flag indicating whether GEDR without picture is enabled. Default: false.
    bool gedrWithoutPictureEnabled_{false};

    /// @brief Flag indicating whether DAQ transmission is enabled. Default: false (safe state).
    bool daqTransmissionEnabled_{false};

    /// @brief File system path to the variant dictionary configuration file.
    std::string variantDictionaryPath_null;

    /// @brief Flag indicating whether the variant code was successfully read from shared memory. Default: false.
    bool variantReadSuccess_{false};
};

} // namespace probe