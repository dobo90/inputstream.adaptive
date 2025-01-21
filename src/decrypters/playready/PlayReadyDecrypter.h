#pragma once
#include "decrypters/IDecrypter.h"

#include <map>
#include <vector>

#include <bento4/Ap4.h>

typedef struct playready_cdm playready_cdm_t;

using namespace DRM;

class CPlayReadyDecrypter : public IDecrypter
{
public:
  CPlayReadyDecrypter() {}
  virtual ~CPlayReadyDecrypter() override;
  virtual std::vector<std::string_view> SelectKeySystems(std::string_view keySystem) override;
  virtual bool OpenDRMSystem(std::string_view licenseURL,
                             const std::vector<uint8_t>& serverCertificate,
                             const uint8_t config) override;
  virtual Adaptive_CencSingleSampleDecrypter* CreateSingleSampleDecrypter(
      std::vector<uint8_t>& initData,
      std::string_view optionalKeyParameter,
      const std::vector<uint8_t>& defaultKeyId,
      std::string_view licenseUrl,
      bool skipSessionMessage,
      CryptoMode cryptoMode) override;
  virtual void DestroySingleSampleDecrypter(Adaptive_CencSingleSampleDecrypter* decrypter) override;
  virtual void GetCapabilities(Adaptive_CencSingleSampleDecrypter* decrypter,
                               const std::vector<uint8_t>& keyid,
                               uint32_t media,
                               DRM::DecrypterCapabilites& caps) override
  {
  }
  virtual bool HasLicenseKey(Adaptive_CencSingleSampleDecrypter* decrypter,
                             const std::vector<uint8_t>& keyid) override;
  virtual bool IsInitialised() override { return m_cdm != nullptr; }
  virtual std::string GetChallengeB64Data(Adaptive_CencSingleSampleDecrypter* decrypter) override
  {
    return "";
  }
  virtual bool OpenVideoDecoder(Adaptive_CencSingleSampleDecrypter* decrypter,
                                const VIDEOCODEC_INITDATA* initData) override
  {
    return false;
  }
  virtual VIDEOCODEC_RETVAL DecryptAndDecodeVideo(kodi::addon::CInstanceVideoCodec* hostInstance,
                                                  const DEMUX_PACKET* sample) override
  {
    return VC_NONE;
  }
  virtual VIDEOCODEC_RETVAL VideoFrameDataToPicture(kodi::addon::CInstanceVideoCodec* hostInstance,
                                                    VIDEOCODEC_PICTURE* picture) override
  {
    return VC_NONE;
  }
  virtual void ResetVideo() override {}
  virtual void SetLibraryPath(std::string_view libraryPath) override
  {
    m_libraryPath = libraryPath;
  }

  virtual bool GetBuffer(void* instance, VIDEOCODEC_PICTURE& picture) { return false; }
  virtual void ReleaseBuffer(void* instance, void* buffer) {}
  virtual std::string_view GetLibraryPath() const override { return m_libraryPath; }

private:
  std::string m_libraryPath;
  std::string m_licenseUrl;
  std::map<std::vector<uint8_t>, std::vector<uint8_t>> m_keys;
  playready_cdm_t* m_cdm{nullptr};
};
