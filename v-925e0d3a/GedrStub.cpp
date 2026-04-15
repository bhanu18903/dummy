#include <cstdint>

namespace probe
{

/**
 * @brief Stub for GEDR write API.
 *
 * This function is used by ProbeComm to upload data to the GEDR backend.
 * On desktop / host builds, we provide a dummy implementation.
 *
 * @return int8_t 0 = success (stubbed)
 */
int8_t GEDR_Write(
    uint32_t clientId,
    const uint8_t* data,
    uint32_t dataSize,
    bool withPicture,
    bool withoutPicture)
{
    (void)clientId;
    (void)data;
    (void)dataSize;
    (void)withPicture;
    (void)withoutPicture;

    // Stub behavior:
    // Return success so ProbeComm continues normally.
    return 0;
}

} // namespace probe