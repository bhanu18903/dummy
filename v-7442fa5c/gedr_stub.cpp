/**
 * Host / WSL stub for GEDR_Write when the GEDR integration library is not linked.
 */
#include <cstdint>

namespace probe {

int8_t GEDR_Write(uint32_t /*offset*/,
                  const uint8_t* /*data*/,
                  uint32_t /*data_size*/,
                  bool /*start_block*/,
                  bool /*end_block*/) noexcept
{
    return 0;
}

}  // namespace probe
