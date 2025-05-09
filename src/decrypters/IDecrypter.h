/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DrmEngineDefines.h"
#include "utils/ResultType.h"

#include <string_view>

#include <kodi/c-api/addon-instance/inputstream/stream_crypto.h>

namespace DRM
{
class IDecrypter
{
public:
  virtual ~IDecrypter() {}

  /*
   * \brief Initialize the decrypter library
   * \return True if has success, otherwise false
   */
  virtual bool Initialize() { return true; }

  /*
   * \brief Initialise the DRM system
   * \param config The DRM configuration
   * \return true on success 
   */
  virtual SResult OpenDRMSystem(const DRM::Config& config) = 0;

  /*
   * \brief Check if the decrypter has been initialised (OpenDRMSystem called)
   * \return True if decrypter has been initialised otherwise false
   */
  virtual bool IsInitialised() = 0;

  /*
   * \brief Set the auxillary library path
   * \param libraryPath Filesystem path for the decrypter to locate any needed files such as CDMs
   */
  virtual void SetLibraryPath(std::string_view libraryPath) = 0;
};
}; // namespace DRM
