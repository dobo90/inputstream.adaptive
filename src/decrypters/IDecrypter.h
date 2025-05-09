/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DrmEngineDefines.h"

#include <string_view>

#include <kodi/c-api/addon-instance/inputstream/stream_crypto.h>

namespace DRM
{
class IDecrypter
{
public:
  virtual ~IDecrypter() {}

  /*
   * \brief Return a short name for the decrypter implementation
   */
  virtual const std::string GetName() const { return "Unknown"; }

  /*
   * \brief Initialize the decrypter library
   * \return True if has success, otherwise false
   */
  virtual bool Initialize() { return true; }

  virtual bool IsKeySystemSupported(std::string_view keySystem) = 0;
};
}; // namespace DRM
