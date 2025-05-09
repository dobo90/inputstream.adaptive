/*
 *  Copyright (C) 2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// \brief AES-128 Key info
struct CAesKeyInfo
{
  std::vector<uint8_t> key;
  std::vector<uint8_t> iv;
  std::string keyUrl;
};
