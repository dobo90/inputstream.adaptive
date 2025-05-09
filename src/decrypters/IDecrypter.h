/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <kodi/c-api/addon-instance/inputstream/stream_crypto.h>

namespace DRM
{
class IDecrypter
{
public:
  virtual ~IDecrypter(){};

  /**
   * \brief Initialize the decrypter library
   * \return True if has success, otherwise false
   */
  virtual bool Initialize() { return true; }

  /**
   * \brief Used to ensure the correct key system is selected
   * \param keySystem The URN to be matched
   * \return Supported URN if type matches to capabilities, otherwise null
   */
  virtual std::vector<std::string_view> SelectKeySystems(std::string_view keySystem) = 0;

  /**
   * \brief Initialise the DRM system
   * \param licenseURL The license URL to contact if applicable
   * \param serverCertificate Server certificate to supply if applicable
   * \param config Flags to be passed to the decrypter
   * \return true on success 
   */
  virtual bool OpenDRMSystem(std::string_view licenseURL,
                             const std::vector<uint8_t>& serverCertificate,
                             const uint8_t config) = 0;

  /**
   * \brief Check if the decrypter has been initialised (OpenDRMSystem called)
   * \return True if decrypter has been initialised otherwise false
   */
  virtual bool IsInitialised() = 0;

  /**
   * \brief Set the auxillary library path
   * \param libraryPath Filesystem path for the decrypter to locate any needed files such as CDMs
   */
  virtual void SetLibraryPath(std::string_view libraryPath) = 0;
};
}; // namespace DRM
