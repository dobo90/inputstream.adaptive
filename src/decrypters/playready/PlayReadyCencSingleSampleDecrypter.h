#include "common/AdaptiveCencSampleDecrypter.h"
#include "decrypters/IDecrypter.h"
#include "playready.h"

#include <optional>

class CPlayReadyCencSingleSampleDecrypter : public Adaptive_CencSingleSampleDecrypter
{
public:
  CPlayReadyCencSingleSampleDecrypter(std::string_view licenseUrl,
                                      std::vector<uint8_t>& initData,
                                      const std::vector<uint8_t>& defaultKeyId,
                                      CryptoMode cryptoMode,
                                      std::map<std::vector<uint8_t>, std::vector<uint8_t>>& cdmKeys,
                                      playready_cdm_t* cdm);

  virtual ~CPlayReadyCencSingleSampleDecrypter();

  virtual AP4_Result SetFragmentInfo(AP4_UI32 poolId,
                                     const std::vector<uint8_t>& keyId,
                                     const AP4_UI08 nalLengthSize,
                                     AP4_DataBuffer& annexbSpsPps,
                                     AP4_UI32 flags,
                                     CryptoInfo cryptoInfo) override
  {
    return AP4_SUCCESS;
  }

  virtual AP4_Result DecryptSampleData(AP4_UI32 poolId,
                                       AP4_DataBuffer& dataIn,
                                       AP4_DataBuffer& dataOut,
                                       const AP4_UI08* iv,
                                       unsigned int subsampleCount,
                                       const AP4_UI16* bytesOfCleartextData,
                                       const AP4_UI32* bytesOfEncryptedData) override;

  virtual void SetDefaultKeyId(const std::vector<uint8_t>& keyId) override {}
  virtual void AddKeyId(const std::vector<uint8_t>& keyId) override {}
  virtual const char* GetSessionId() override { return m_strSession.c_str(); }

  bool HasKeyId(const std::vector<uint8_t>& keyId);

private:
  static void GetKeysFromCdm(std::string_view licenseUrl,
                             std::vector<uint8_t>& initData,
                             const std::vector<uint8_t>& keyId,
                             playready_cdm_t* cdm,
                             std::map<std::vector<uint8_t>, std::vector<uint8_t>>& cdmKeys);

  AP4_CencSingleSampleDecrypter* m_singleSampleDecrypter{nullptr};
  std::string m_strSession;
  std::optional<std::vector<uint8_t>> m_keyId;
};
