#include "Cdm.h"

#include <mutex>

using namespace DRM;

std::optional<std::vector<uint8_t>> Cdm::GetKey(const std::vector<uint8_t>& kid)
{
  std::shared_lock<std::shared_mutex> lock(m_keysMutex);

  auto it = m_keys.find(kid);
  return it != std::end(m_keys) ? std::make_optional<>(it->second) : std::nullopt;
}

void Cdm::AddKey(std::vector<uint8_t> kid, std::vector<uint8_t> key)
{
  std::unique_lock<std::shared_mutex> lock(m_keysMutex);
  m_keys[kid] = key;
}