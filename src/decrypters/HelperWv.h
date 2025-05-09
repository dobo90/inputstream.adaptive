/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#ifdef INPUTSTREAM_TEST_BUILD
#include "test/KodiStubs.h"
#else
#include <kodi/AddonBase.h>
#endif

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// forward
namespace DRM
{
struct Config;
}

namespace DRM
{

/*!
 * \brief Generate a synthesized Widevine PSSH.
 *        (WidevinePsshData as google protobuf format
 *        https://github.com/devine-dl/pywidevine/blob/master/pywidevine/license_protocol.proto)
 * \param kid The KeyId
 * \param contentIdData Custom content for the "content_id" field as bytes
 *                      Placeholders allowed:
 *                      {KID} To inject the KID as bytes
 *                      {UUID} To inject the KID as UUID string format
 * \return The pssh if has success, otherwise empty value.
 */
std::vector<uint8_t> MakeWidevinePsshData(const std::vector<std::vector<uint8_t>>& keyIds,
                                          std::vector<uint8_t> contentIdData);

/*!
 * \brief Generate a synthesized Widevine PSSH.
 *        (WidevinePsshData as google protobuf format
 *        https://github.com/devine-dl/pywidevine/blob/master/pywidevine/license_protocol.proto)
 * \param kid The KeyId
 * \param contentIdData Custom content for the "content_id" field as bytes
 *                      Placeholders allowed:
 *                      {KID} To inject the KID as bytes
 *                      {UUID} To inject the KID as UUID string format
 * \return The pssh if has success, otherwise empty value.
 */
void ParseWidevinePssh(const std::vector<uint8_t>& wvPsshData,
                       std::vector<std::vector<uint8_t>>& keyIds);

bool WvWrapLicense(std::string& data,
                   const std::vector<uint8_t>& challenge,
                   std::string_view sessionId,
                   const std::vector<uint8_t>& kid,
                   const std::vector<uint8_t>& pssh,
                   std::string_view wrapper,
                   const bool isNewConfig);

bool WvUnwrapLicense(std::string_view wrapper,
                     const std::map<std::string, std::string>& params,
                     std::string_view contentType,
                     std::string data,
                     std::string& dataOut);

void TranslateLicenseUrlPh(std::string& url,
                           const std::vector<uint8_t>& challenge,
                           const bool isNewConfig);

/*!
 * \brief Parse the value of "X-Limit-Video" HTTP header.
 * \param value The header value
 * \return The value of "max" parameter, otherwise 0
 */
int ParseXLimitVideoHeader(std::string_view value);

} // namespace DRM
