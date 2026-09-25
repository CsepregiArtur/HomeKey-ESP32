#include "DiscoveryAdvertiser.hpp"

#include "ConfigManager.hpp"
#include "DeviceCert.hpp"
#include "NodeIdentityManager.hpp"
#include "WebServerManager.hpp"
#include "config.hpp"
#include "defaults.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"

#include <array>
#include <string>
#include <vector>

namespace {

const char *TAG = "Discovery";

constexpr const char *kService = "_homekey";
constexpr const char *kProto = "_tcp";

/// Bumped only when the REST contract changes in a way an older client cannot handle,
/// so a component can refuse a device instead of misreading it.
constexpr const char *kProtocolVersion = "1";

/// HomeSpan drops mDNS on its reset paths; re-check often enough to recover quickly but
/// not so often that the lock/socket work is repeated for no reason.
constexpr int64_t kRecheckIntervalUs = 30 * 1000000LL;

/// mdns_hostname_get() requires a buffer of MDNS_NAME_BUF_LEN (64) chars.
constexpr size_t kHostnameBufLen = MDNS_NAME_BUF_LEN;

/// Appended to the instance name so two HomeKey devices on one LAN stay distinguishable.
std::string macSuffix() {
  std::array<uint8_t, 6> mac{};
  if (esp_read_mac(mac.data(), ESP_MAC_WIFI_STA) != ESP_OK) {
    return "";
  }
  char buf[7];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  return buf;
}

} // namespace

bool DiscoveryAdvertiser::isStationMode() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK) {
    return false;
  }
  // HTTPS is disabled in AP mode, so advertising the API there would point clients at a
  // plaintext, captive-portal-only endpoint.
  return mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
}

void DiscoveryAdvertiser::begin(ConfigManager &configManager,
                                NodeIdentityManager &nodeIdentityManager,
                                WebServerManager &webServerManager) {
  m_configManager = &configManager;
  m_nodeIdentityManager = &nodeIdentityManager;
  m_webServerManager = &webServerManager;

  const espConfig::https_certs_t &certs = configManager.getHttpsCertsConfig();
  m_fingerprint = deviceCert::certificateFingerprint(certs.serverCert);

  const espConfig::misc_config_t &misc =
      configManager.getConfig<espConfig::misc_config_t>();
  const std::string baseName = misc.deviceName.empty() ? DEVICE_NAME : misc.deviceName;
  const std::string suffix = macSuffix();
  m_instanceName = suffix.empty() ? baseName : baseName + "-" + suffix;

  advertise();
}

bool DiscoveryAdvertiser::advertise() {
  if (!isStationMode()) {
    ESP_LOGD(TAG, "Not advertising: the API is only served on the station interface");
    return false;
  }

  // Idempotent - it returns ESP_OK when the stack is already up, which is the normal
  // case because HomeSpan starts mDNS for HAP during homekitLock->begin().
  if (mdns_init() != ESP_OK) {
    ESP_LOGW(TAG, "Could not initialise mDNS; the device will not be discoverable");
    return false;
  }

  // The hostname is left alone when one is already set: HomeSpan owns it, it is what
  // HomeKit controllers resolve, and changing it could strand existing pairings.
  std::array<char, kHostnameBufLen> hostname{};
  if (mdns_hostname_get(hostname.data()) != ESP_OK || hostname[0] == '\0') {
    std::string generated = m_instanceName;
    if (mdns_hostname_set(generated.c_str()) != ESP_OK) {
      ESP_LOGW(TAG, "Could not set an mDNS hostname");
      return false;
    }
    ESP_LOGI(TAG, "Set mDNS hostname to %s", generated.c_str());
  }

  const uint16_t port = m_webServerManager == nullptr ? 0 : m_webServerManager->getServerPort();
  if (port == 0) {
    // The web server has not finished starting, so there is nothing to point at yet.
    // refresh() will retry.
    ESP_LOGD(TAG, "Not advertising: the web server has no port yet");
    return false;
  }

  if (mdns_service_exists(kService, kProto, nullptr)) {
    m_advertised = true;
    return true;
  }

  const espConfig::misc_config_t &misc =
      m_configManager->getConfig<espConfig::misc_config_t>();
  const household::NodeInfo &node = m_nodeIdentityManager->info();

  // Key/value strings are duplicated by the stack, so these locals only have to outlive
  // the mdns_service_add() call below. Reserved up front so the c_str() pointers handed
  // to the stack cannot be invalidated by a reallocation.
  std::vector<std::string> values;
  values.reserve(8);
  values.emplace_back(node.node_id.empty() ? m_instanceName : node.node_id); // id
  values.emplace_back(node.node_name.empty() ? m_instanceName : node.node_name); // name
  values.emplace_back(misc.deviceName.empty() ? DEVICE_NAME : misc.deviceName);  // model
  values.emplace_back(esp_app_get_description()->version);                       // ver
  values.emplace_back(kProtocolVersion);                                         // proto
  values.emplace_back(m_fingerprint.empty() ? "" : m_fingerprint);               // fp
  values.emplace_back("rw");                                                     // cfg
  values.emplace_back(m_webServerManager->isTlsActive() ? "1" : "0");            // tls

  static const char *kKeys[] = {"id", "name", "model", "ver", "proto", "fp", "cfg", "tls"};
  std::vector<mdns_txt_item_t> txt;
  txt.reserve(values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    txt.push_back({kKeys[i], values[i].c_str()});
  }

  const esp_err_t err =
      mdns_service_add(m_instanceName.c_str(), kService, kProto, port, txt.data(), txt.size());
  if (err != ESP_OK) {
    // ESP_ERR_INVALID_ARG here means the service already exists - not a failure.
    if (mdns_service_exists(kService, kProto, nullptr)) {
      m_advertised = true;
      return true;
    }
    ESP_LOGW(TAG, "Could not advertise %s: %s", kService, esp_err_to_name(err));
    return false;
  }

  m_advertised = true;
  ESP_LOGW(TAG, "Advertising %s on port %u (%s, cfg=rw, fingerprint %s)",
           kService, static_cast<unsigned>(port),
           m_webServerManager->isTlsActive() ? "TLS" : "PLAINTEXT",
           m_fingerprint.empty() ? "unavailable" : m_fingerprint.c_str());
  if (!m_webServerManager->isTlsActive()) {
    ESP_LOGW(TAG, "API is advertised without TLS - clients should refuse to configure it");
  }
  return true;
}

void DiscoveryAdvertiser::refresh() {
  if (m_advertised) {
    return;
  }
  const int64_t now = esp_timer_get_time();
  if (m_lastAttemptUs != 0 && now - m_lastAttemptUs < kRecheckIntervalUs) {
    return;
  }
  m_lastAttemptUs = now;
  advertise();
}

void DiscoveryAdvertiser::onNetworkUp() {
  // The station interface coming up is exactly when the web server gets a port, so the
  // retry throttle would only delay discovery for no reason.
  m_lastAttemptUs = 0;
  advertise();
}

void DiscoveryAdvertiser::stop() {
  if (!m_advertised) {
    return;
  }
  mdns_service_remove(kService, kProto);
  m_advertised = false;
  m_lastAttemptUs = 0;
}
