/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DrmFactory.h"

#include "playready/PlayReadyCdm.h"
#include "widevine/WidevineCdm.h"

#include <kodi/c-api/addon-instance/inputstream/stream_crypto.h>

using namespace DRM;

Cdm* DRM::FACTORY::GetCdm(STREAM_CRYPTO_KEY_SYSTEM keySystem)
{
  if (keySystem == STREAM_CRYPTO_KEY_SYSTEM_WIDEVINE)
  {
    return new WidevineCdm();
  }
  else if (keySystem == STREAM_CRYPTO_KEY_SYSTEM_PLAYREADY)
  {
    return new PlayreadyCdm();
  }

  return nullptr;
}
