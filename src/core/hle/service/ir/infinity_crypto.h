// Disney Infinity Figure Encryption/Decryption
// Add this header to your project and include it where needed
// Based on Dolphin Emulator implementation

#pragma once

#include <array>
#include <string>
#include "common/common_types.h"

namespace InfinityCrypto {

// Decrypt figure ID from a 320-byte figure file
// Returns the figure ID (e.g., 0x0F4241 for Mr. Incredible)
u32 DecryptFigureId(const std::array<u8, 320>& file_data);

// Create a new figure file with proper encryption
// file_path: where to save the .bin file
// figure_num: the figure ID (e.g., 0x0F4241 for Mr. Incredible)
// Returns true on success
bool CreateFigure(const std::string& file_path, u32 figure_num);

} // namespace InfinityCrypto
