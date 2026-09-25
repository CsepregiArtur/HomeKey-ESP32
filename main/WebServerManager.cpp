// ============================================================================
// WebServerManager.cpp - ESP32 Web Server Implementation
// ============================================================================

#include "GPIOAllocator.hpp"
#include "esp_https_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "app_event_loop.hpp"
#include "app_events.hpp"
#include "EthernetDriver.hpp"
#include "fmt/ranges.h"
#include "WebServerManager.hpp"
#include "ConfigManager.hpp"
#include "HomeSpan.h"
#include "MqttManager.hpp"
#include "NfcManager.hpp"
#include "ReaderDataManager.hpp"
#include "HouseholdManager.hpp"
#include "NodeIdentityManager.hpp"
#include "SecurityManager.hpp"
#include "HealthManager.hpp"
#include "AuditManager.hpp"
#include "ProvisioningManager.hpp"
#include "BackupManager.hpp"
#include "RestoreManager.hpp"
#include "LockManager.hpp"
#include "DeviceCert.hpp"
#include "cJSON.h"
// Must come after the Arduino headers. Arduino's IPAddress.h expands lwip's
// INADDR_NONE (which is IPADDR_NONE, i.e. `((u32_t)0xffffffffUL)`), so if lwip is
// pulled in first that expansion produces `const IPAddress ((u32_t)0xffffffffUL)(0,0,0,0)`
// and the header fails to parse. Including esp_http_client.h last means Arduino has
// already consumed the macro by the time the HTTP client drags lwip in.
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "config.hpp"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_log_level.h"
#include "esp_wifi.h"
#include "eth_structs.hpp"
#include "eventStructs.hpp"
#include "freertos/idf_additions.h"
#include "loggable.hpp"
#include "portmacro.h"
#include "sodium/randombytes.h"
#include <LittleFS.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <esp_app_desc.h>
#include <esp_netif.h>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <nvs.h>
#include <esp_tls_crypto.h>
#include <stdbool.h>
#include <string>
#include <vector>
#include "JsonGuard.hpp"
#include "json_escape.hpp"

// ============================================================================
// Constants
// ============================================================================

const char *WebServerManager::TAG = "WebServerManager";
const size_t MAX_WS_PAYLOAD = 8192;
const size_t HEAP_UPPER_THRESHOLD = 70 * 1000;
const size_t HEAP_LOWER_THRESHOLD = 50 * 1000;

// ============================================================================
// Helper Functions
// ============================================================================

static inline bool str_ends_with(const char *str, const char *suffix) {
  if (!str || !suffix)
    return false;
  size_t lenstr = strlen(str), lensuf = strlen(suffix);
  return lenstr >= lensuf && memcmp(str + lenstr - lensuf, suffix, lensuf) == 0;
}

inline std::optional<std::string> check_pin_reassignment(uint8_t incoming_pin,
                                                         uint8_t current_pin,
                                                         const std::string& key,
                                                         int array_index,
                                                         bool override_strapping) {
    if (incoming_pin == current_pin || incoming_pin == 255) return std::nullopt;

    GPIOAllocator::PinRole role = GPIOAllocator::PinRole::GpioOut;
    GPIOAllocator::PinConsumer consumer = GPIOAllocator::PinConsumer::Hardware;
    bool output_capable = true;
    if (key == "nfcGpioPins") {
      consumer = GPIOAllocator::PinConsumer::Nfc;
      switch (array_index) {
        case 0: role = GPIOAllocator::PinRole::SpiCs;   break; // SS / SDA
        case 1: role = GPIOAllocator::PinRole::SpiSck;  break; // SCK / SCL
        case 2: role = GPIOAllocator::PinRole::SpiMiso; break;
        case 3: role = GPIOAllocator::PinRole::SpiMosi; break;
      }
    } else if (key == "nfcIrqPin") {
      consumer = GPIOAllocator::PinConsumer::Nfc;
      role = GPIOAllocator::PinRole::NfcIrq;
    } else if (key == "nfcVenPin") {
      consumer = GPIOAllocator::PinConsumer::Nfc;
      role = GPIOAllocator::PinRole::NfcVen;
    } else if (key == "ethSpiConfig") {
      consumer = GPIOAllocator::PinConsumer::Eth;
      switch (array_index) {
        case 1: role = GPIOAllocator::PinRole::SpiCs;   break; // CS
        case 2: role = GPIOAllocator::PinRole::EthIrq;  break; // IRQ
        case 3: role = GPIOAllocator::PinRole::EthRst;  break; // RST
        case 4: role = GPIOAllocator::PinRole::SpiSck;  break; // SCK
        case 5: role = GPIOAllocator::PinRole::SpiMiso; break;
        case 6: role = GPIOAllocator::PinRole::SpiMosi; break;
      }
    } else if (key == "controlPin") {
      consumer = GPIOAllocator::PinConsumer::HomeKit;
      role = GPIOAllocator::PinRole::GpioIn;
      output_capable = false;
    } else if (key == "hsStatusPin") {
      consumer = GPIOAllocator::PinConsumer::HomeKit;
      role = GPIOAllocator::PinRole::Led;
    } else if (key == "nfcSuccessPin" || key == "nfcFailPin" ||
               key == "tagEventPin" || key == "hkAltActionInitLedPin") {
      consumer = GPIOAllocator::PinConsumer::Hardware;
      role = GPIOAllocator::PinRole::Led;
    } else if (key == "hkAltActionInitPin") {
      consumer = GPIOAllocator::PinConsumer::Hardware;
      role = GPIOAllocator::PinRole::Irq;
      output_capable = false;
    }

    auto status = GPIOAllocator::instance().status_of(incoming_pin);
    if (status.strapping && status.holders.empty()) {
      if (override_strapping) return std::nullopt;
      return std::string("is a strapping pin and strapping override is disabled");
    }
    auto verdict = GPIOAllocator::instance().validate(
        gpio_num_t(incoming_pin),
        output_capable ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
        role, consumer);
    if (verdict) return std::nullopt;
    return std::string(GPIOAllocator::error_str(verdict.error())) +
           " (currently held by: " +
           GPIOAllocator::instance().owner_of(incoming_pin).value_or("unknown") + ")";
}

// ============================================================================
// Constructor & Destructor
/**
 * @brief Construct a WebServerManager and initialize internal references and defaults.
 *
 * Stores references to the provided ConfigManager and ReaderDataManager, initializes
 * the internal HTTP server pointer and MQTT manager pointer to null.
 *
 * @param configManager Reference to the configuration manager used for reading and saving settings.
 * @param readerDataManager Reference to the reader data manager used for accessing reader-related state.
 */

WebServerManager::WebServerManager(ConfigManager &configManager,
                                   NvsCredentialStore &readerDataManager)
    : m_server(nullptr), m_configManager(configManager),
      m_readerDataManager(readerDataManager), m_mqttManager(nullptr), m_nfcManager(nullptr) {
}

/**
 * @brief Clean up WebServerManager resources on destruction.
 *
 * Performs orderly shutdown of server-related subsystems and frees associated resources.
 *
 * @details Calls the OTA cleanup routine, stops the HTTP server if it is running, and
 * stops and deletes the periodic status timer.
 */
WebServerManager::~WebServerManager() {
  ESP_LOGI(TAG, "WebServerManager destructor called");

  if (m_server) {
    httpd_stop(m_server);
    m_server = nullptr;
  }
  if (m_statusTimer) {
    esp_timer_stop(m_statusTimer);
    esp_timer_delete(m_statusTimer);
    m_statusTimer = nullptr;
  }
}

bool WebServerManager::shouldEnableHttps() const {
    const auto& miscConfig = m_configManager.getConfig<espConfig::misc_config_t>();
    if (!miscConfig.webHttpsEnabled) {
        return false;
    }

    bool isMqttSslEnabled = m_configManager.getConfig<espConfig::mqttConfig_t>().useSSL;

    size_t freeHeap = esp_get_free_heap_size();

    if (isMqttSslEnabled && freeHeap < HEAP_UPPER_THRESHOLD) {
        ESP_LOGW(TAG, "HTTPS Web UI degraded to HTTP: Free heap (%zu bytes) insufficient for both HTTPS and MQTT SSL.", freeHeap);
        return false;
    }

    if (freeHeap < HEAP_LOWER_THRESHOLD) {
        ESP_LOGW(TAG, "HTTPS Web UI degraded to HTTP: Free heap (%zu bytes) below minimum threshold.", freeHeap);
        return false;
    }

    return true;
}

esp_err_t WebServerManager::sendJsonError(httpd_req_t *req, const std::string &msg,
                                      const char *status) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, status);
  std::string response = JsonBuilder::object()
      .addBool("success", false)
      .addString("error", msg.c_str())
      .toStringUnformatted();
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

bool WebServerManager::readBody(httpd_req_t *req, std::string &out, size_t maxSize) {
  if (req->content_len <= 0) {
    sendJsonError(req, "Empty request body", "400 Bad Request");
    return false;
  }
  if (static_cast<size_t>(req->content_len) > maxSize) {
    sendJsonError(req,
                  fmt::format("Request body is {} bytes; the most this endpoint accepts is {}.",
                              req->content_len, maxSize),
                  "413 Payload Too Large");
    return false;
  }

  out.assign(static_cast<size_t>(req->content_len), '\0');
  size_t received = 0;
  while (received < out.size()) {
    const int chunk = httpd_req_recv(req, out.data() + received, out.size() - received);
    if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (chunk <= 0) {
      sendJsonError(req, "Could not read the request body", "400 Bad Request");
      return false;
    }
    received += static_cast<size_t>(chunk);
  }
  return true;
}

std::string ownerConflictMsg(int pin, const std::string &key, const std::string &owner) {
  return std::to_string(pin) + " for \"" + key + "\" already owned by \"" + owner + "\".";
}

bool WebServerManager::heapGuardOk(httpd_req_t *req, bool otherActive,
                                    const char *thisName, const char *otherName) {
  size_t freeHeap = esp_get_free_heap_size();
  if (otherActive && freeHeap < HEAP_UPPER_THRESHOLD) {
    sendJsonError(req, std::string(thisName) + " cannot be enabled while " + otherName + " is active (low RAM).");
    return false;
  }
  if (freeHeap < HEAP_LOWER_THRESHOLD) {
    sendJsonError(req, std::string(thisName) + " cannot be enabled due to insufficient free heap memory.");
    return false;
  }
  return true;
}

// ============================================================================
// Initialization
/**
 * @brief Initialize and start the web server, WebSocket subsystem, and related resources.
 *
 * Initializes a new session identifier, mounts LittleFS, configures and starts the HTTP server,
 * creates the WebSocket send queue and worker task, registers HTTP/WebSocket routes, and
 * creates a periodic status timer. On failure of critical steps the method logs the error
 * and aborts initialization (server, queue, or task pointers may remain null).
 */

void WebServerManager::begin() {
  ESP_LOGI(TAG, "Initializing...");

  std::vector<uint8_t> sessionIdBytes(32);
  randombytes_buf(sessionIdBytes.data(), sessionIdBytes.size());
  m_sessionId = fmt::format("{:02x}", fmt::join(sessionIdBytes, ""));

  if (!LittleFS.begin()) {
    ESP_LOGW(TAG, "Failed to mount LittleFS");
  } else{
    ESP_LOGI(TAG, "LittleFS mounted: %d/%d bytes", LittleFS.usedBytes(),
            LittleFS.totalBytes());
    // Assets are stored compressed (brotli or gzip, depending on the release that
    // built the image). Neither present means the filesystem image was never
    // flashed - worth saying out loud, because the Web UI is then a blank 404.
    if (!LittleFS.exists("/index.html.br") && !LittleFS.exists("/index.html.gz")) {
      ESP_LOGW(TAG, "No web UI assets in the filesystem partition; flash the littlefs "
                    "image (see docs/content/updates.md).");
    }
  }
  wifi_mode_t currentMode;
  esp_err_t wifiErr = esp_wifi_get_mode(&currentMode);
  bool isApMode = (wifiErr == ESP_OK && (currentMode == WIFI_MODE_AP || currentMode == WIFI_MODE_APSTA));
  bool isHttpsActive = !isApMode && shouldEnableHttps();

  httpd_ssl_config_t ssl_config = HTTPD_SSL_CONFIG_DEFAULT();
  // Must be at least as large as the largest route table below, otherwise the tail of
  // the table is silently dropped: httpd_register_uri_handler logs "no slots left" and
  // the handler never exists. That used to leave the catch-all `{"/*"}` unregistered,
  // so every unmatched URL returned "Nothing matches the given URI" (404) - including
  // the Web UI root and the captive-portal redirect.  //
  // setupRoutes() registers 34 handlers; setupCaptivePortalRoutes() registers 10. Both
  // tables can end up on the same server across an AP/STA transition, so size for the
  // sum with headroom rather than for either table alone.
  ssl_config.httpd.max_uri_handlers = 56;
  ssl_config.httpd.max_open_sockets = 4;
  ssl_config.httpd.stack_size = 6144;
  ssl_config.httpd.uri_match_fn = httpd_uri_match_wildcard;
  ssl_config.httpd.lru_purge_enable = true;
  ssl_config.httpd.backlog_conn = 4;

  if (!isHttpsActive) {
    ssl_config.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;
    ESP_LOGI(TAG, "Starting Web Server in HTTP mode");
  } else {
    ESP_LOGI(TAG, "Starting Web Server in HTTPS mode");
  }

  if (isHttpsActive) {
    const auto& httpsCerts = m_configManager.getHttpsCertsConfig();
    if (!httpsCerts.serverCert.empty() && !httpsCerts.privateKey.empty()) {
      ssl_config.servercert = reinterpret_cast<const uint8_t *>(httpsCerts.serverCert.c_str());
      ssl_config.servercert_len = httpsCerts.serverCert.length() + 1;
      ssl_config.prvtkey_pem = reinterpret_cast<const uint8_t *>(httpsCerts.privateKey.c_str());
      ssl_config.prvtkey_len = httpsCerts.privateKey.length() + 1;
      if (!httpsCerts.caCert.empty()) {
        ssl_config.cacert_len = httpsCerts.caCert.length() + 1;
        ssl_config.cacert_pem = reinterpret_cast<const uint8_t *>(httpsCerts.caCert.c_str());
      }
      ESP_LOGI(TAG, "Loaded user HTTPS certificates (%d bytes cert, %d bytes key)",
              httpsCerts.serverCert.length(), httpsCerts.privateKey.length());
    } else ESP_LOGI(TAG, "No user HTTPS certificates found");
  }

  // httpd_ssl_start() sets httpd.server_port from port_secure or port_insecure depending
  // on transport_mode, so read it back afterwards rather than assuming 443/80.
  if (httpd_ssl_start(&m_server, &ssl_config) == ESP_OK) {
    m_tlsActive.store(isHttpsActive);
    m_serverPort.store(ssl_config.httpd.server_port);
    ESP_LOGI(TAG, "HTTP server started on port %u, free heap: %zu",
             static_cast<unsigned>(ssl_config.httpd.server_port), esp_get_free_heap_size());
  } else {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    // Keeping the Web UI reachable for a human is worth the downgrade, but the API is
    // then unencrypted. isTlsActive() reports that so callers can refuse to use it.
    ssl_config.transport_mode = HTTPD_SSL_TRANSPORT_INSECURE;
    if (httpd_ssl_start(&m_server, &ssl_config) == ESP_OK) {
      m_tlsActive.store(false);
      m_serverPort.store(ssl_config.httpd.server_port);
      ESP_LOGW(TAG, "HTTP server started (INSECURE) on port %u",
               static_cast<unsigned>(ssl_config.httpd.server_port));
    } else {
      ESP_LOGE(TAG, "Failed to start HTTP server in INSECURE mode as well!");
      return;
    }
  }
  // With TLS active the main server only listens on 443, so a plain http:// address to the
  // device stops working altogether. Keep port 80 open purely to send those clients to the
  // right place, which is what makes enabling HTTPS non-breaking for bookmarks.
  if (m_tlsActive.load(std::memory_order_relaxed)) {
    startHttpsRedirectServer();
  }

  m_wsQueue = xQueueCreate(64, sizeof(WsFrame *));  if (!m_wsQueue) {
    ESP_LOGE(TAG, "Failed to create WebSocket queue");
    httpd_stop(m_server);
    m_server = nullptr;
    return;
  }
BaseType_t task;
#ifndef CONFIG_FREERTOS_UNICORE
    task = xTaskCreatePinnedToCore(ws_send_task, "ws_send_task", 4096, this, 2,
                      &m_wsTaskHandle, 1);
#else
    task = xTaskCreate(ws_send_task, "ws_send_task", 4096, this, 2,
                      &m_wsTaskHandle);
#endif
  if (task != pdPASS) {
    ESP_LOGE(TAG, "Failed to create WebSocket task");
    vQueueDelete(m_wsQueue);
    httpd_stop(m_server);
    m_server = nullptr;
    return;
  }

  if (isApMode) {
    setupCaptivePortalRoutes();
  } else {
    setupRoutes();
  }

  esp_timer_create_args_t timerArgs = {
      .callback = &statusTimerCallback, .arg = this, .name = "statusTimer"};
  if (esp_timer_create(&timerArgs, &m_statusTimer) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create status timer");
  }

  ESP_LOGI(TAG, "Web server initialization complete");

  m_isInitialized = true;
}

/**
 * @brief Stops the web server and cleans up all resources.
 *
 * Performs a complete shutdown of the web server by stopping the HTTP server,
 * deleting the WebSocket task and queue, and stopping/deleting the status timer.
 */
void WebServerManager::end() {
  ESP_LOGI(TAG, "Ending WebServerManager...");

  if(!m_isInitialized) return;

  if (m_server) {
    httpd_ssl_stop(m_server);
    ESP_LOGI(TAG, "HTTP Server stopped!");
    m_server = nullptr;
    // Nothing is listening any more, so stop reporting a port and a transport that no
    // longer exist - anything advertising this API would otherwise point at a dead port.
    m_serverPort.store(0);
    m_tlsActive.store(false);
  }

  if (m_redirectServer) {
    httpd_stop(m_redirectServer);
    m_redirectServer = nullptr;
  }

  if (m_wsTaskHandle) {
    vTaskDelete(m_wsTaskHandle);
    m_wsTaskHandle = nullptr;
  }

  if (m_wsQueue) {
    WsFrame* frame = nullptr;
    while (xQueueReceive(m_wsQueue, &frame, 0) == pdPASS) {
      if (frame) {
        if (frame->payload != frame->inlinePayload) {
          delete[] frame->payload;
        }
        delete frame;
      }
    }
    vQueueDelete(m_wsQueue);
    m_wsQueue = nullptr;
  }

  if (m_statusTimer) {
    esp_timer_stop(m_statusTimer);
    esp_timer_delete(m_statusTimer);
    m_statusTimer = nullptr;
  }

  ESP_LOGI(TAG, "WebServerManager ended");
}

namespace {

/// Branch-free comparison of fixed-size secrets, so the login check cannot leak a
/// matching prefix through response timing.
bool constantTimeEquals(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) {
    return false;
  }
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

/// True when the value looks like an IPv4 literal ("192.168.4.1", not a host name).
bool isIpv4Literal(const std::string &value) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char trailing = '\0';
  if (sscanf(value.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &trailing) != 4) {
    return false;
  }
  return a < 256 && b < 256 && c < 256 && d < 256;
}

std::string toLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

/** A static asset as it is actually stored, plus the encoding it is stored in. */
struct AssetVariant {
  std::string path;        ///< Path inside LittleFS; empty when the asset is missing
  const char *encoding;    ///< Value for Content-Encoding, nullptr when stored uncompressed
};

/**
 * @brief Read the encodings the client accepts from the Accept-Encoding header.
 *
 * A substring match is deliberate: "br" also matches "brotli", which some clients
 * send, and being generous here only means a compressed variant is chosen when it
 * exists.
 */
void parseAcceptEncoding(httpd_req_t *req, bool &acceptBrotli, bool &acceptGzip) {
  acceptBrotli = false;
  acceptGzip = false;
  const size_t len = httpd_req_get_hdr_value_len(req, "Accept-Encoding");
  if (len == 0 || len >= 256) {
    return;
  }
  char header[256];
  if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", header, sizeof(header)) != ESP_OK) {
    return;
  }
  acceptBrotli = strstr(header, "br") != nullptr;
  acceptGzip = strstr(header, "gzip") != nullptr;
}

/**
 * @brief Pick the best stored variant of a static asset.
 *
 * The web UI ships pre-compressed: brotli from the version that ran out of space in
 * the littlefs partition, gzip before that. Which one an image contains therefore
 * depends on the firmware version that built it, and the two can be updated
 * independently (firmware via OTA, assets via the LittleFS upload). Trying both
 * keeps every combination working, and the uncompressed fallback covers images
 * built without compression at all.
 *
 * When the client cannot decode the only available variant, the file is still
 * served with its real Content-Encoding header: a 404 would be strictly worse, and
 * a client that cannot decode brotli could not render that asset regardless.
 */
AssetVariant resolveAsset(const std::string &path, bool acceptBrotli, bool acceptGzip) {
  const bool haveBrotli = LittleFS.exists((path + ".br").c_str());
  const bool haveGzip = LittleFS.exists((path + ".gz").c_str());
  if (haveBrotli && acceptBrotli) {
    return {path + ".br", "br"};
  }
  if (haveGzip && acceptGzip) {
    return {path + ".gz", "gzip"};
  }
  if (haveBrotli) {
    return {path + ".br", "br"};
  }
  if (haveGzip) {
    return {path + ".gz", "gzip"};
  }
  if (LittleFS.exists(path.c_str())) {
    return {path, nullptr};
  }
  return {"", nullptr};
}

/**
 * @brief True for the URIs whose handlers only accept POST.
 *
 * They fall through to the single-page-app catch-all handler for any other method,
 * which would answer a GET with the app shell and status 200 - indistinguishable from
 * a successful call for anything scripting against the API. The query string is
 * ignored, so "/reset_hk_pair?x=1" is matched too.
 */
bool isStateChangingEndpoint(const char *uri) {
  static constexpr std::array<const char *, 4> kPaths = {
      "/reboot_device", "/reset_hk_pair", "/reset_wifi_cred", "/start_config_ap"};
  for (const char *path : kPaths) {
    const size_t len = strlen(path);
    if (strncmp(uri, path, len) == 0 && (uri[len] == '\0' || uri[len] == '?')) {
      return true;
    }
  }
  return false;
}

} // namespace

/**
 * @brief Reject requests whose Host header does not name this device.
 *
 * The whole API is meant to be reached directly on the local network, but a browser
 * does not know that: a hostile page can point a host name it controls at the
 * device's IP address and then read the answers, because from the browser's point of
 * view the request is same-origin (DNS rebinding). The Host header is the part an
 * attacker cannot forge, so only IP literals, "localhost" and names that resolve
 * on-link (mDNS ".local") are accepted.
 */
bool WebServerManager::hostHeaderAllowed(httpd_req_t *req) {
  const size_t len = httpd_req_get_hdr_value_len(req, "Host");
  if (len == 0) {
    return true; // HTTP/1.0 request without a Host header (cannot be a rebinding attack)
  }
  if (len >= 64) {
    return false;
  }
  char host[64];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    return false;
  }
  std::string name(host);
  const size_t colon = name.find(':');
  if (colon != std::string::npos) {
    name.erase(colon); // strip the port
  }
  name = toLowerCopy(name);
  if (name.empty()) {
    return false;
  }
  if (isIpv4Literal(name) || name == "localhost") {
    return true;
  }
  if (name.front() == '[') {
    return true; // IPv6 literal such as [::1]
  }
  // mDNS names are answered on-link only, so accepting them does not open the
  // rebinding hole: a remote attacker cannot answer an mDNS query.
  if (name.size() > 6 && name.compare(name.size() - 6, 6, ".local") == 0) {
    return true;
  }
  for (const char *ifKey : {"WIFI_STA_DEF", "WIFI_AP_DEF"}) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifKey);
    if (!netif) {
      continue;
    }
    const char *netifHostname = nullptr;
    if (esp_netif_get_hostname(netif, &netifHostname) == ESP_OK &&
        netifHostname != nullptr && netifHostname[0] != '\0') {
      const std::string candidate = toLowerCopy(netifHostname);
      if (name == candidate) {
        return true;
      }
    }
  }
  ESP_LOGW(TAG, "Rejected request with unexpected Host header '%s'", name.c_str());
  return false;
}

bool WebServerManager::basicAuth(httpd_req_t* req){
  if(!hostHeaderAllowed(req)){
    return false;
  }
  // The setup portal runs before any Wi-Fi (and therefore any Web UI credentials)
  // exist, and it is already gated by the setup AP password. Requiring Web UI auth
  // here would lock a user who lost the password out of their own device, since the
  // portal is the only way back in.
  if(m_captivePortalMode || !m_configManager.getConfig<espConfig::misc_config_t>().webAuthEnabled){
    return true;
  }
  size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
  if(!(hdr_len > 0)){
    ESP_LOGD(TAG, "Authorization data not provided");
    return false;
  }
  std::string authReq; authReq.resize(hdr_len + 1);
  if(httpd_req_get_hdr_value_str(req, "Authorization", authReq.data(), authReq.size()) != ESP_OK){
    ESP_LOGD(TAG, "Invalid HTTP Header, authorization failed");
    return false;
  }
  const std::string cred = fmt::format("{}:{}", m_configManager.getConfig<espConfig::misc_config_t>().webUsername, m_configManager.getConfig<espConfig::misc_config_t>().webPassword);
  size_t n = 0;
  esp_crypto_base64_encode(NULL, 0, &n, (const uint8_t*)cred.c_str(), cred.size());
  std::string digest = "Basic ";
  digest.resize(6+n);
  esp_crypto_base64_encode((uint8_t *)digest.data() + 6, digest.size(), &n, (const uint8_t *)cred.c_str(), cred.size());
  if (constantTimeEquals(authReq, digest)) {
    m_authFailureCount = 0;
    return true;
  }
  // The credentials are static, so guessing only needs time. Slow it down without
  // introducing a persistent lockout that a user could get stuck in.
  if (++m_authFailureCount >= 5) {
    ESP_LOGW(TAG, "%lu failed Web UI logins - delaying the response",
             static_cast<unsigned long>(m_authFailureCount));
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  return false;
}

// ============================================================================
// Route Setup
// ============================================================================

esp_err_t WebServerManager::ws_post_handshake_cb(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance) {
    return ESP_FAIL;
  }

  int sockfd = httpd_req_to_sockfd(req);
  ESP_LOGI(TAG, "WebSocket connection established: fd=%d", sockfd);
  instance->addWebSocketClient(sockfd);

  // Send initial device status & metrics
  std::string status = instance->getDeviceInfo();
  instance->queue_ws_frame(sockfd, (const uint8_t *)status.c_str(),
                           status.size(), HTTPD_WS_TYPE_TEXT);
  std::string metrics = instance->getDeviceMetrics();
  instance->queue_ws_frame(sockfd, (const uint8_t *)metrics.c_str(),
                           metrics.size(), HTTPD_WS_TYPE_TEXT);

  if (!esp_timer_is_active(instance->m_statusTimer)) {
    esp_timer_start_periodic(instance->m_statusTimer, 5000 * 1000);
  }

  if(!instance->m_wsBroadcastBuffer.empty()){
    for (auto &v : instance->m_wsBroadcastBuffer) {
      instance->queue_ws_frame(sockfd, v.data(), v.size(), HTTPD_WS_TYPE_TEXT);
    }
    instance->m_wsBroadcastBuffer.clear();
  }

  return ESP_OK;
}

void WebServerManager::setupRoutes() {
  ESP_LOGI(TAG, "Setting up routes...");

  m_captivePortalMode = false;

  struct RouteConfig {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    void *ctx;
    bool is_ws = false;
  };

  RouteConfig routes[] = {
      // Static files
      {"/static/*", HTTP_GET, handleStaticFiles, this},
      {"/assets/*", HTTP_GET, handleStaticFiles, this},

      // Configuration endpoints
      {"/config", HTTP_GET, handleGetConfig, this},
      {"/config/clear", HTTP_POST, handleClearConfig, this},
      {"/config/save", HTTP_POST, handleSaveConfig, this},
      {"/eth_get_config", HTTP_GET, handleGetEthConfig, this},
      {"/nfc_get_presets", HTTP_GET, handleGetNfcPresets, this},

      // Action endpoints. State changes are POST-only: a plain GET is reachable
      // from any page the user happens to visit, which must not be able to reset
      // the HomeKit pairing or force the device into setup mode (CSRF).
      {"/reboot_device", HTTP_POST, handleReboot, this},
      {"/reset_hk_pair", HTTP_POST, handleHKReset, this},
      {"/reset_wifi_cred", HTTP_POST, handleWifiReset, this},
      {"/start_config_ap", HTTP_POST, handleStartConfigAP, this},

      // WebSocket
      {"/ws", HTTP_GET, handleWebSocket, this, true},

      // OTA endpoints. The two GitHub-update routes must be registered *before* the
      // `/ota/*` wildcard: handlers are matched in registration order, and the wildcard
      // would otherwise swallow them and treat the request as a firmware upload.
      {"/ota/release", HTTP_GET, handleGetReleaseInfo, this},
      {"/ota/install", HTTP_POST, handleInstallRelease, this},
      {"/ota/*", HTTP_POST, handleOTAUpload, this},

      // Certificate endpoints
      {"/certificates", HTTP_POST, handleCertificateUpload, this},
      {"/certificates", HTTP_GET, handleCertificateStatus, this},
      {"/certificates", HTTP_DELETE, handleCertificateDelete, this},

      // Household / node / backup / recovery / provisioning endpoints
      {"/household", HTTP_GET, handleGetHousehold, this},
      {"/node", HTTP_GET, handleGetNode, this},
      // Names a paired HomeKit controller. POST-only: it is a state change, and a GET
      // could be triggered by any page the user happens to visit.
      {"/issuer/name", HTTP_POST, handleSetIssuerName, this},
      {"/health", HTTP_GET, handleGetHealth, this},
      {"/security", HTTP_GET, handleGetSecurity, this},
      {"/audit", HTTP_GET, handleGetAudit, this},
      {"/backup", HTTP_GET, handleGetBackup, this},
      {"/backup/create", HTTP_POST, handleCreateBackup, this},
      {"/backup/restore", HTTP_POST, handleRestoreBackup, this},
      {"/recovery/export", HTTP_POST, handleExportRecovery, this},
      {"/provision/issue", HTTP_POST, handleIssueProvisioning, this},
      {"/provision/join", HTTP_POST, handleJoinHousehold, this},

      // Home Assistant direct API. Kept under /api/ so it can never collide with a Web
      // UI page name, and registered before the catch-all.
      {"/api/ha/info", HTTP_GET, handleHaInfo, this},
      {"/api/ha/state", HTTP_GET, handleHaState, this},
      {"/api/ha/config", HTTP_GET, handleHaConfig, this},
      {"/api/ha/config", HTTP_POST, handleHaConfig, this},
      // POST-only: a state change reachable by simply visiting a URL could be driven by
      // any page the user happens to have open.
      {"/api/ha/lock", HTTP_POST, handleHaLock, this},

      // Catch-all (must be last)
      {"/*", HTTP_GET, handleRootOrHash, this}};

  for (auto &r : routes) {
    httpd_uri_t uri = {.uri = r.uri,
                       .method = r.method,
                       .handler = r.handler,
                       .user_ctx = r.ctx};
#ifdef CONFIG_HTTPD_WS_SUPPORT
    uri.is_websocket = r.is_ws;
    if (r.is_ws) {
      uri.ws_post_handshake_cb = ws_post_handshake_cb;
    }
#endif
    esp_err_t err = httpd_register_uri_handler(m_server, &uri);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register URI %s: %s", r.uri, esp_err_to_name(err));
    }
  }
  ESP_LOGI(TAG, "Routes setup complete");
}

// ============================================================================
// Captive Portal Route Setup
// ============================================================================

void WebServerManager::setupCaptivePortalRoutes() {
  ESP_LOGI(TAG, "Setting up captive portal routes...");

  // Marked before the handlers are registered so that basicAuth() never demands
  // Web UI credentials from requests that can only come from the setup portal.
  m_captivePortalMode = true;

  struct RouteConfig {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    void *ctx;
  };

  RouteConfig routes[] = {
      // Captive portal API endpoints
      {"/captive_portal_config", HTTP_GET, handleGetCaptivePortalConfig, this},
      {"/captive_portal_config", HTTP_POST, handleSaveCaptivePortalConfig, this},
      {"/nfc_get_presets", HTTP_GET, handleGetNfcPresets, this},
      {"/eth_get_config", HTTP_GET, handleGetEthConfig, this},
      {"/wifi_scan", HTTP_GET, handleWifiScan, this},
      {"/reboot_device", HTTP_POST, handleReboot, this},
      // Static files needed for the captive portal UI
      {"/static/*", HTTP_GET, handleStaticFiles, this},
      {"/assets/*", HTTP_GET, handleStaticFiles, this},

      // The captive portal page itself
      {"/captive-portal", HTTP_GET, handleRootOrHash, this},

      // Catch-all redirect to captive portal (must be last)
      {"/*", HTTP_GET, handleCaptivePortal, this}};

  for (auto &r : routes) {
    httpd_uri_t uri = {.uri = r.uri,
                       .method = r.method,
                       .handler = r.handler,
                       .user_ctx = r.ctx,
                       .is_websocket = false,
                       .handle_ws_control_frames = false,
                       .supported_subprotocol = nullptr};
    esp_err_t err = httpd_register_uri_handler(m_server, &uri);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register captive portal URI %s: %s", r.uri, esp_err_to_name(err));
    }
  }
  ESP_LOGI(TAG, "Captive portal routes setup complete");
}

// ============================================================================
// Utility Methods
// ============================================================================

WebServerManager *WebServerManager::getInstance(httpd_req_t *req) {
  return static_cast<WebServerManager *>(req->user_ctx);
}

esp_err_t WebServerManager::sendAuthFailure(httpd_req_t *req) {
  ESP_LOGE(TAG, "HTTP Authorization failed!");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Polaris\"");
  httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NULL);
  return ESP_OK;
}

// ============================================================================
// Static File Handler
// ============================================================================

esp_err_t WebServerManager::handleStaticFiles(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  const char *last_slash = strrchr(req->uri, '/');
  const char *filename = last_slash ? last_slash + 1 : req->uri;
  std::string requested = req->uri;
  if (strlen(filename) == 0){
    // A directory-style URI is the single-page app: serve its entry document.
    filename = "index.html";
    requested = "/index.html";
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  } else {
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
  }

  bool acceptBrotli = false, acceptGzip = false;
  parseAcceptEncoding(req, acceptBrotli, acceptGzip);
  const AssetVariant asset = resolveAsset(requested, acceptBrotli, acceptGzip);
  if (asset.path.empty()) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  File file = LittleFS.open(asset.path.c_str(), "r");
  if (!file) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  // Determine content type
  const char *content_type = "text/plain";
  if (str_ends_with(filename, ".html"))
    content_type = "text/html";
  else if (str_ends_with(filename, ".css"))
    content_type = "text/css";
  else if (str_ends_with(filename, ".js"))
    content_type = "application/javascript";
  else if (str_ends_with(filename, ".json"))
    content_type = "application/json";
  else if (str_ends_with(filename, ".png"))
    content_type = "image/png";
  else if (str_ends_with(filename, ".jpg") || str_ends_with(filename, ".jpeg"))
    content_type = "image/jpeg";
  else if (str_ends_with(filename, ".ico"))
    content_type = "image/x-icon";
  else if (str_ends_with(filename, ".webp"))
    content_type = "image/webp";

  httpd_resp_set_type(req, content_type);
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  if (asset.encoding != nullptr)
    httpd_resp_set_hdr(req, "Content-Encoding", asset.encoding);

  char *buffer = (char*)malloc(4096); 
  if (!buffer) {
      file.close();
      return ESP_ERR_NO_MEM;
  }

  size_t bytes_read;
  esp_err_t err = ESP_OK;
  while ((bytes_read = file.read((uint8_t*)buffer, 4096)) > 0) {
      err = httpd_resp_send_chunk(req, buffer, bytes_read);
      vTaskDelay(pdMS_TO_TICKS(5));
      if (err != ESP_OK) break;
  }

  free(buffer);
  file.close();
  if (err != ESP_OK) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TLS send error");
      return err;
  }
  return httpd_resp_send_chunk(req, NULL, 0); // End chunked stream cleanly
}

/**
 * @brief Serve the single-page application entry (app.html) for root/hash requests.
 *
 * Authenticates the request, issues a sessionId cookie when different from the server's session,
 * sets appropriate response headers, and streams /app.html from LittleFS in chunks.
 *
 * @param req The HTTP request to handle.
 * @return esp_err_t ESP_OK on successful send; ESP_FAIL if authentication fails, the file is missing, or a send error occurs.
 */
esp_err_t WebServerManager::handleRootOrHash(httpd_req_t *req) {
  WebServerManager* instance = getInstance(req);
  char sessionId[65];
  size_t sessionIdLen = sizeof(sessionId);
  esp_err_t err = httpd_req_get_cookie_val(req, "sessionId", sessionId, &sessionIdLen);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  // A state-changing endpoint reached with the wrong method must not render the app.
  if (isStateChangingEndpoint(req->uri)) {
    httpd_resp_set_hdr(req, "Allow", "POST");
    httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "This endpoint only accepts POST");
    return ESP_OK;
  }
  std::string sessionCookie;
  if(instance->m_sessionId.compare(sessionId) != 0 || err != ESP_OK){
    sessionCookie = fmt::format("sessionId={};", instance->m_sessionId);
    httpd_resp_set_hdr(req, "Set-Cookie", sessionCookie.c_str());
  }

  bool acceptBrotli = false, acceptGzip = false;
  parseAcceptEncoding(req, acceptBrotli, acceptGzip);
  const AssetVariant entry = resolveAsset("/index.html", acceptBrotli, acceptGzip);
  if (entry.path.empty()) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  File file = LittleFS.open(entry.path.c_str(), "r");
  if (!file) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  if (entry.encoding != nullptr)
    httpd_resp_set_hdr(req, "Content-Encoding", entry.encoding);

  char buffer[1024];
  size_t bytes_read;
  while ((bytes_read = file.read((uint8_t *)buffer, sizeof(buffer))) > 0) {
    if (httpd_resp_send_chunk(req, buffer, bytes_read) != ESP_OK) {
      file.close();
      httpd_resp_send_chunk(req, NULL, 0);
      return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  file.close();
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// ============================================================================
// Configuration Handlers
// ============================================================================

esp_err_t WebServerManager::handleGetConfig(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_OK;
  }

  char query[256], type_param[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "type", type_param, sizeof(type_param)) !=
          ESP_OK) {
    return sendJsonError(req, "Missing 'type' parameter");
  }

  std::string type = type_param;
  JsonGuard dataGuard(nullptr);

  if (type == "mqtt") {
    std::string s = instance->m_configManager.serializeToJson<espConfig::mqttConfig_t>();
    dataGuard.reset(cJSON_Parse(s.c_str()));
  } else if (type == "misc") {
    std::string s = instance->m_configManager.serializeToJson<espConfig::misc_config_t>();
    dataGuard.reset(cJSON_Parse(s.c_str()));
  } else if (type == "actions"){
    std::string s = instance->m_configManager.serializeToJson<espConfig::actions_config_t>();
    dataGuard.reset(cJSON_Parse(s.c_str()));
  } else if (type == "hkinfo") {
    const auto readerData = instance->m_readerDataManager.snapshot();
    JsonGuard hkInfo(cJSON_CreateObject());
    cJSON_AddStringToObject(hkInfo.get(), "group_identifier", fmt::format("{:02X}", fmt::join(readerData.identity.group_identifier, "")).c_str());
    cJSON_AddStringToObject(hkInfo.get(), "unique_identifier", fmt::format("{:02X}", fmt::join(readerData.identity.sub_identifier, "")).c_str());

    JsonGuard issuersArray(cJSON_CreateArray());
    for (const auto &issuer : readerData.issuers) {
      JsonGuard issuerJson(cJSON_CreateObject());
      cJSON_AddStringToObject(issuerJson.get(), "issuerId", fmt::format("{:02X}", fmt::join(issuer.id, "")).c_str());
      // Always present, empty when unnamed: a stable shape is easier for a client than
      // a field that appears only sometimes, and an empty string says "the user has not
      // named this" rather than inventing a placeholder.
      cJSON_AddStringToObject(issuerJson.get(), "name",
                              instance->m_readerDataManager.issuerLabel(issuer.id).c_str());
      
      JsonGuard endpointsArray(cJSON_CreateArray());
      for (const auto &endpoint : issuer.endpoints) {
        JsonGuard ep(cJSON_CreateObject());
        cJSON_AddStringToObject(ep.get(), "endpointId", fmt::format("{:02X}", fmt::join(endpoint.id, "")).c_str());
        cJSON_AddItemToArray(endpointsArray.get(), ep.release());
      }
      cJSON_AddItemToObject(issuerJson.get(), "endpoints", endpointsArray.release());
      cJSON_AddItemToArray(issuersArray.get(), issuerJson.release());
    }
    cJSON_AddItemToObject(hkInfo.get(), "issuers", issuersArray.release());
    dataGuard = std::move(hkInfo);
  } else {
    return sendJsonError(req, "Invalid 'type' parameter");
  }

  httpd_resp_set_type(req, "application/json");
  std::string response = JsonBuilder::object()
      .addBool("success", true)
      .addItem("data", std::move(dataGuard))
      .toStringUnformatted();
  
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t WebServerManager::handleGetNfcPresets(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  JsonBuilder presets = JsonBuilder::object();
  presets.withArray("presets", [&](JsonBuilder& presetsArray) {
    for (auto &&v : nfcGpioPinsPresets) {
      JsonBuilder preset = JsonBuilder::object();
      preset.addString("name", v.name.c_str());
      preset.addNumber("type", v.type);
      preset.withArray("gpioPins", [&](JsonBuilder& gpioArray) {
        for (auto &&pin : v.gpioPins) {
          gpioArray.addItemToArray(JsonGuard(cJSON_CreateNumber(pin)));
        }
      });
      preset.addNumber("irqPin", v.irqPin);
      preset.addNumber("venPin", v.venPin);
      presetsArray.addItemToArray(std::move(preset).release());
    }
  });
  JsonBuilder response = JsonBuilder::object();
  response.addItem("data", std::move(presets).release());
  response.addBool("success", true);

  std::string resp = response.toStringUnformatted();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t WebServerManager::handleGetEthConfig(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  JsonBuilder eth_config = JsonBuilder::object();

  // Supported chips
  eth_config.withArray("supportedChips", [&](JsonBuilder& chipsArray) {
    for (auto &&v : eth_config_ns::supportedChips) {
      JsonBuilder chip = JsonBuilder::object();
      chip.addString("name", v.second.name.c_str());
      chip.addBool("emac", v.second.emac);
      chip.addNumber("phy_type", v.second.phy_type);
      chipsArray.addItemToArray(std::move(chip).release());
    }
  });

  // Board presets
  eth_config.withArray("boardPresets", [&](JsonBuilder& boardPresetsArray) {
    for (auto &&v : eth_config_ns::boardPresets) {
      JsonBuilder preset = JsonBuilder::object();
      preset.addString("name", v.name.c_str());

      preset.withObject("ethChip", [&](JsonBuilder& chip) {
        chip.addString("name", v.ethChip.name.c_str());
        chip.addBool("emac", v.ethChip.emac);
        chip.addNumber("phy_type", v.ethChip.phy_type);
      });

      if(v.ethChip.emac){
#if CONFIG_ETH_USE_ESP32_EMAC
        preset.withObject("rmii_conf", [&](JsonBuilder& rmii_conf) {
          rmii_conf.addNumber("phy_addr", v.rmii_conf.phy_addr);
          rmii_conf.addNumber("pin_mcd", v.rmii_conf.pin_mcd);
          rmii_conf.addNumber("pin_mdio", v.rmii_conf.pin_mdio);
          rmii_conf.addNumber("pin_power", v.rmii_conf.pin_power);
          rmii_conf.addNumber("pin_rmii_clock", v.rmii_conf.pin_rmii_clock);
        });
#endif
      } else {
        preset.withObject("spi_conf", [&](JsonBuilder& spi_conf) {
          spi_conf.addNumber("spi_freq_mhz", v.spi_conf.spi_freq_mhz);
          spi_conf.addNumber("pin_cs", v.spi_conf.pin_cs);
          spi_conf.addNumber("pin_irq", v.spi_conf.pin_irq);
          spi_conf.addNumber("pin_rst", v.spi_conf.pin_rst);
          spi_conf.addNumber("pin_sck", v.spi_conf.pin_sck);
          spi_conf.addNumber("pin_miso", v.spi_conf.pin_miso);
          spi_conf.addNumber("pin_mosi", v.spi_conf.pin_mosi);
        });
      }
      boardPresetsArray.addItemToArray(std::move(preset).release());
    }
  });

  eth_config.addBool("ethEnabled", instance->m_configManager.getConfig<espConfig::misc_config_t>().ethernetEnabled);
  eth_config.addNumber("numSpiBuses", SPI_HOST_MAX - 1);

  httpd_resp_set_type(req, "application/json");
  JsonBuilder response = JsonBuilder::object();
  response.addBool("success", true);
  response.addItem("data", std::move(eth_config).release());
  
  std::string resp = response.toStringUnformatted();
  httpd_resp_send(req, resp.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

/**
 * @brief Handle an HTTP request to save a configuration object for a given config type.
 *
 * Processes the request's "type" query parameter and JSON body, validates the payload
 * against the current configuration schema, applies updates, persists the configuration,
 * publishes relevant configuration change events, and sends an appropriate JSON HTTP response.
 * May trigger a device reboot when certain configuration keys change.
 *
 * @param req The HTTP request containing the query parameter `type=<mqtt|misc|actions>` and
 *            a JSON body with the configuration fields to update.
 * @return esp_err_t `ESP_OK` if the configuration was saved and applied; `ESP_FAIL` otherwise.
 */
esp_err_t WebServerManager::handleSaveConfig(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_OK;
  }

  char query[256], type_param[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "type", type_param, sizeof(type_param)) != ESP_OK) {
    return sendJsonError(req, "Missing 'type' parameter");
  }

  const size_t max_content_size = 2048;
  if (req->content_len >= max_content_size) {
    return sendJsonError(req, "Request body too large", "413 Payload Too Large");
  }

  std::vector<char> content(max_content_size, 0);
  int ret = httpd_req_recv(req, content.data(), content.size() - 1);
  if (ret <= 0) {
    return ESP_FAIL;
  }
  content[ret] = '\0';

  JsonGuard obj(cJSON_Parse(content.data()));
  if (!obj) {
    instance->sendJsonError(req, "Invalid JSON");
    return ESP_OK;
  }

  std::string type = type_param;
  JsonGuard configSchema(nullptr);

  if (type == "mqtt") {
    std::string s = instance->m_configManager.serializeToJson<espConfig::mqttConfig_t>();
    configSchema.reset(cJSON_Parse(s.c_str()));
  } else if (type == "misc") {
    std::string s = instance->m_configManager.serializeToJson<espConfig::misc_config_t>();
    configSchema.reset(cJSON_Parse(s.c_str()));
  } else if (type == "actions") {
    std::string s = instance->m_configManager.serializeToJson<espConfig::actions_config_t>();
    configSchema.reset(cJSON_Parse(s.c_str()));
  } else {
    return sendJsonError(req, "Invalid 'type' parameter");
  }

  if (!validateRequest(req, configSchema.get(), obj.get())) {
    return ESP_OK; // validateRequest already sent a full error response
  }

  bool success = false, rebootNeeded = false;
  std::string rebootMsg, errorMsg;

  cJSON *it = obj.get()->child;
  if (it == NULL) {
    return sendJsonError(req, "Received empty object, nothing to save");
  }
  
  // Safe string representation that cleans itself up
  std::string data_str = to_string_unformatted(obj);
  std::string result;
  
  while (it) {
    cJSON *configSchemaItem = cJSON_GetObjectItem(configSchema.get(), it->string);
    if (cJSON_Compare(it, configSchemaItem, true)) {
      it = it->next;
      continue;
    }

    const std::string keyStr = it->string;

    if (keyStr == "setupCode") {
      EventValueChanged s{.name = keyStr, .str = it->valuestring};
      std::vector<uint8_t> d;
      alpaca::serialize(s, d);
      HomekitEvent event{.type = HomekitEventType::SETUP_CODE_CHANGED, .data = d};
      std::vector<uint8_t> event_data;
      alpaca::serialize(event, event_data);
      AppEventLoop::publish(HK_EVENT, HK_INTERNAL_EVENT, event_data.data(), event_data.size());
    } else if (keyStr == "nfcNeopixelPin") {
      rebootNeeded = true;
      rebootMsg = "Pixel GPIO pin changed, reboot needed! Rebooting...";
    } else if (str_ends_with(keyStr.c_str(), "Pin")) {
      EventValueChanged s{.name = keyStr, .oldValue = (uint8_t)configSchemaItem->valueint, .newValue = (uint8_t)it->valueint};
      std::vector<uint8_t> d;
      alpaca::serialize(s, d);
      AppEventLoop::publish(HW_EVENT, HW_CONFIG_CHANGED, d.data(), d.size());
      
      if (keyStr == "gpioActionPin" && it->valueint != 255) {
        cJSON* dumbSwitch = cJSON_GetObjectItem(obj.get(), "hkDumbSwitchMode");
        if (dumbSwitch && cJSON_IsTrue(dumbSwitch)) {
          cJSON_SetBoolValue(dumbSwitch, false); // Mutates in-place safely
        }
      }
    } else if (keyStr == "btrLowStatusThreshold") {
      EventValueChanged s{.name = "btrLowThreshold", .newValue = (uint8_t)it->valueint};
      std::vector<uint8_t> d;
      alpaca::serialize(s, d);
      HomekitEvent event{.type = HomekitEventType::BTR_PROP_CHANGED, .data = d};
      std::vector<uint8_t> event_data;
      alpaca::serialize(event, event_data);
      AppEventLoop::publish(HK_EVENT, HK_INTERNAL_EVENT, event_data.data(), event_data.size());
    } else if (keyStr == "neoPixelType") {
      rebootNeeded = true;
      rebootMsg = "Pixel Type changed, reboot needed! Rebooting...";
    }
    it = it->next;
  }

  if (type == "mqtt") {
    result = instance->m_configManager.updateFromJson<espConfig::mqttConfig_t>(data_str);
    if (!result.empty()) {
      success = instance->m_configManager.saveConfig<espConfig::mqttConfig_t>();
      rebootNeeded = true;
      rebootMsg = "MQTT config saved, reboot needed! Rebooting...";
    }
  } else if (type == "misc") {
    result = instance->m_configManager.updateFromJson<espConfig::misc_config_t>(data_str);
    if (!result.empty()) {
      success = instance->m_configManager.saveConfig<espConfig::misc_config_t>();
      rebootNeeded = true;
      rebootMsg = "Misc config saved, reboot needed! Rebooting...";
    }
  } else if (type == "actions") {
    result = instance->m_configManager.updateFromJson<espConfig::actions_config_t>(data_str);
    if (!result.empty()) {
      success = instance->m_configManager.saveConfig<espConfig::actions_config_t>();
    }
  }

  httpd_resp_set_type(req, "application/json");
  if (success) {
    JsonBuilder res = JsonBuilder::object();
    res.addBool("success", true);
    res.addString("message", rebootNeeded ? rebootMsg.c_str() : "Saved and applied!");
    JsonGuard dataPtr(cJSON_Parse(result.c_str()));
    res.addItem("data", std::move(dataPtr));
    
    std::string response = res.toStringUnformatted();
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    if (rebootNeeded) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
    }
  } else {
    JsonBuilder res = JsonBuilder::object();
    res.addBool("success", false);
    res.addString("error", errorMsg.empty() ? "Unable to save config!" : errorMsg.c_str());
    httpd_resp_set_status(req, HTTPD_500);
    std::string response = res.toStringUnformatted();
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  }
  return ESP_OK;
}

bool WebServerManager::validateRequest(httpd_req_t *req, cJSON *currentData, cJSON *obj) {
  bool overrideStrapping = false;
  cJSON *ovrStrItem = cJSON_GetObjectItem(obj, "overrideStrappingRestriction");
  if (!ovrStrItem) ovrStrItem = cJSON_GetObjectItem(currentData, "overrideStrappingRestriction");

  if (ovrStrItem) {
    overrideStrapping = cJSON_IsBool(ovrStrItem) && cJSON_IsTrue(ovrStrItem);
  } else {
    overrideStrapping = getInstance(req)->m_configManager.getConfig<espConfig::misc_config_t>().overrideStrappingRestriction;
  }

  cJSON *readerTypeItem = cJSON_GetObjectItem(obj, "nfcReaderType");
  const uint8_t effectiveReaderType =
      (readerTypeItem && cJSON_IsNumber(readerTypeItem))
          ? static_cast<uint8_t>(readerTypeItem->valueint)
          : getInstance(req)->m_configManager.getConfig<espConfig::misc_config_t>().nfcReaderType;

  cJSON *it = obj->child;
  while (it) {
    std::string keyStr = it->string;
    cJSON *existingValue = cJSON_GetObjectItem(currentData, keyStr.c_str());

    if (!existingValue) {
      std::string msg = "\"" + keyStr + "\" is not a valid configuration key.";
      sendJsonError(req, msg);
      return false;
    }

    cJSON *incomingValue = it;
    bool typeOk = false;
    if (cJSON_IsString(existingValue))
      typeOk = cJSON_IsString(incomingValue);
    else if (cJSON_IsObject(existingValue))
      typeOk = cJSON_IsObject(incomingValue);
    else if (cJSON_IsArray(existingValue))
      typeOk = cJSON_IsArray(incomingValue);
    else if (cJSON_IsBool(existingValue))
      typeOk = cJSON_IsBool(incomingValue) ||
               (cJSON_IsNumber(incomingValue) &&
                (incomingValue->valueint == 0 || incomingValue->valueint == 1));
    else if (cJSON_IsNumber(existingValue))
      typeOk = cJSON_IsNumber(incomingValue);

    if (!typeOk) {
      char *valueStr = cJSON_PrintUnformatted(incomingValue);
      std::unique_ptr<char, decltype(&cJSON_free)> valueStrGuard(valueStr, &cJSON_free);
      std::string msg = "Invalid type for key \"" + keyStr + "\". Received: " + std::string(valueStr ? valueStr : "null");
      sendJsonError(req, msg);
      return false;
    }

    // Setup code validation
    if (keyStr == "setupCode") {
      // Type check above already guarantees cJSON_IsString
      std::string code = incomingValue->valuestring;
      if (code.length() != 8 ||
          std::find_if(code.begin(), code.end(), [](unsigned char c) {
            return !std::isdigit(c);
          }) != code.end()) {
        std::string msg =
            "\"" + code + "\" is not valid. Must be an 8-digit number.";
        sendJsonError(req, msg);
        return false;
      }
      static constexpr std::array<const char*, 12> kWeakCodes = {
        "00000000","11111111","22222222","33333333","44444444","55555555",
        "66666666","77777777","88888888","99999999","12345678","87654321"
      };
      if (std::find(kWeakCodes.begin(), kWeakCodes.end(), code) != kWeakCodes.end()) {
        sendJsonError(req, "\"" + code + "\" is too simple to use as a Setup Code.");
        return false;
      }
      if (homeSpan.controllerListBegin() != homeSpan.controllerListEnd() &&
          code.compare(cJSON_GetStringValue(existingValue)) != 0) {
        sendJsonError(req, "Setup Code can only be set if no devices are paired");
        return false;
      }
    }
    // Pin validation
    else if (str_ends_with(keyStr.c_str(), "Pin")) {
      // IRQ/VEN only exist on the PN7161 reader; for other reader types the
      // values are meaningless and must not fail validation.
      const bool nfcReaderPins = keyStr == "nfcIrqPin" || keyStr == "nfcVenPin";
      if (nfcReaderPins && effectiveReaderType != 1) {
        it = it->next;
        continue;
      }

      // Reject anything outside uint8_t range BEFORE truncating, so a value
      // like 256 can't wrap to a valid-looking pin (0) and slip past both
      // the GPIO-validity check and the ownership check below.
      if (incomingValue->valueint < 0 || incomingValue->valueint > 255) {
        std::string msg = std::to_string(incomingValue->valueint) +
                          " is not a valid GPIO Pin for \"" + keyStr + "\".";
        sendJsonError(req, msg);
        return false;
      }

      const uint8_t incomingPin = static_cast<uint8_t>(incomingValue->valueint);

      // Fix: Should use || instead of && because output is a strict subset of input.
      if (incomingPin != 255 && (!GPIO_IS_VALID_GPIO(incomingPin) ||
                                 !GPIO_IS_VALID_OUTPUT_GPIO(incomingPin))) {
        std::string msg = std::to_string(incomingPin) +
                          " is not a valid GPIO Pin for \"" + keyStr + "\".";
        sendJsonError(req, msg);
        return false;
      }

      const uint8_t currentPin   = (cJSON_IsNumber(existingValue) &&
                                    existingValue->valueint >= 0 &&
                                    existingValue->valueint <= 255)
                                        ? static_cast<uint8_t>(existingValue->valueint)
                                        : uint8_t{255};
      if (auto error = check_pin_reassignment(incomingPin, currentPin, keyStr, -1, overrideStrapping)) {
        std::string msg = std::to_string(incomingPin) + " for \"" + keyStr + "\" " + *error + ".";
        sendJsonError(req, msg);
        return false;
      }
    } else if (keyStr == "ethSpiBus" && cJSON_IsNumber(incomingValue) && (incomingValue->valueint < SPI2_HOST || incomingValue->valueint >= SPI_HOST_MAX)){
        std::string msg = std::to_string(incomingValue->valueint) +
                      " is not a valid SPI Bus value";
        sendJsonError(req, msg);
        return false;
    } else if ((str_ends_with(keyStr.c_str(), "Pins") || str_ends_with(keyStr.c_str(), "SpiConfig")) && cJSON_IsArray(incomingValue)){
      cJSON *currentArr = cJSON_GetObjectItem(currentData, keyStr.c_str());
      cJSON *el = NULL;
      int idx = 0;
      cJSON_ArrayForEach(el, incomingValue) {
        if (cJSON_IsNumber(el)) {
          if (idx == 0 && keyStr == "ethSpiConfig") { idx++; continue; }

          // Reject out-of-range values before truncating so a wrapped value
          // can't slip past the ownership lookup below.
          if (el->valueint < 0 || el->valueint > 255) {
            std::string msg = std::to_string(el->valueint) +
                              " is not a valid GPIO Pin for \"" + keyStr + "\".";
            sendJsonError(req, msg);
            return false;
          }
          const uint8_t elPin = static_cast<uint8_t>(el->valueint);

          uint8_t currentPin = 255;
          if (currentArr && cJSON_IsArray(currentArr)) {
            cJSON *ce = cJSON_GetArrayItem(currentArr, idx);
            if (ce && cJSON_IsNumber(ce) && ce->valueint >= 0 && ce->valueint <= 255)
              currentPin = static_cast<uint8_t>(ce->valueint);
          }
          if (auto error = check_pin_reassignment(elPin, currentPin, keyStr, idx, overrideStrapping)) {
            std::string msg = std::to_string(elPin) + " for \"" + keyStr + "\" " + *error + ".";
            sendJsonError(req, msg);
            return false;
          }
        }
        idx++;
      }
    }
    // --- Heap Memory Guard Checks ---
    if (keyStr == "webHttpsEnabled" && cJSON_IsTrue(it)) {
      bool mqttSsl = getInstance(req)->m_configManager.getConfig<espConfig::mqttConfig_t>().useSSL;
      if (!heapGuardOk(req, mqttSsl, "HTTPS", "MQTT SSL")) { return false; }
    } else if (keyStr == "useSSL" && cJSON_IsTrue(it)) {
      bool https = getInstance(req)->m_configManager.getConfig<espConfig::misc_config_t>().webHttpsEnabled;
      if (!heapGuardOk(req, https, "MQTT SSL", "HTTPS")) { return false; }
    }

    if (cJSON_IsBool(existingValue) && cJSON_IsNumber(incomingValue)) {
      cJSON_SetBoolValue(incomingValue, incomingValue->valueint);
    }
    
    it = it->next;
  }
  
  return true;
}

esp_err_t WebServerManager::handleClearConfig(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  char query[256], type_param[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "type", type_param, sizeof(type_param)) != ESP_OK) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    std::string response = JsonBuilder::object()
        .addBool("success", false)
        .addString("error", "Missing 'type' parameter")
        .toStringUnformatted();
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  std::string type = type_param;
  bool success = false;
  if (type == "mqtt")        success = instance->m_configManager.deleteConfig<espConfig::mqttConfig_t>();
  else if (type == "misc")   success = instance->m_configManager.deleteConfig<espConfig::misc_config_t>();
  else if (type == "actions")success = instance->m_configManager.deleteConfig<espConfig::actions_config_t>();

  if (success) {
    httpd_resp_send(req, "Cleared! Rebooting...", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
  }
  httpd_resp_send_500(req);
  return ESP_FAIL;
}

// ============================================================================
// Action Handlers
// ============================================================================

esp_err_t WebServerManager::handleReboot(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (instance && !instance->basicAuth(req)) {
    return sendAuthFailure(req);
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"success\":\"true\",\"message\":\"Rebooting...\"}");
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
  return ESP_OK;
}

esp_err_t WebServerManager::handleHKReset(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "application/json");
  std::string response = JsonBuilder::object()
      .addBool("success", true)
      .addString("message", "Erasing HomeKit pairings, device will reboot")
      .toStringUnformatted();
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  instance->m_readerDataManager.deleteAllReaderData();
  homeSpan.processSerialCommand("H");
  return ESP_OK;
}

esp_err_t WebServerManager::handleWifiReset(httpd_req_t *req) {
  WebServerManager* instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  httpd_resp_set_type(req, "application/json");
  std::string response = JsonBuilder::object()
      .addBool("success", true)
      .addString("message", "Erasing WiFi credentials, device will reboot")
      .toStringUnformatted();
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  homeSpan.processSerialCommand("X");
  return ESP_OK;
}

esp_err_t WebServerManager::handleStartConfigAP(httpd_req_t *req) {
  WebServerManager* instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  httpd_resp_set_type(req, "application/json");
  std::string response = JsonBuilder::object()
      .addBool("success", true)
      .addString("message", "Starting AP mode...")
      .toStringUnformatted();
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  vTaskDelay(pdMS_TO_TICKS(1000));
  auto run = [](void* p){ homeSpan.processSerialCommand("A"); vTaskDelete(nullptr); };
  xTaskCreate(run, "hs_cmd", 4096, NULL, 5, nullptr);
  return ESP_OK;
}

// ============================================================================
// Captive Portal
// ============================================================================

esp_err_t WebServerManager::handleCaptivePortal(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance || !instance->basicAuth(req)) {
    return sendAuthFailure(req);
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/captive-portal");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

esp_err_t WebServerManager::handleGetCaptivePortalConfig(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (!instance->basicAuth(req)) {
    return sendAuthFailure(req);
  }

  const auto &miscConfig = instance->m_configManager.getConfig<espConfig::misc_config_t>();

  JsonBuilder config = JsonBuilder::object();
  config.addString("setupCode", miscConfig.setupCode.c_str());
  // The portal lets the user set up Web UI authentication; the stored password is
  // deliberately not part of this payload. On a device that has not been through
  // first-run setup there is no usable password yet - the setup screen is where the
  // user chooses one.
  config.addBool("webAuthEnabled", miscConfig.webAuthEnabled);
  config.addString("webUsername", miscConfig.webUsername.c_str());
  config.addNumber("hk_key_color", miscConfig.hk_key_color);
  config.addNumber("nfcPinsPreset", miscConfig.nfcPinsPreset);
  
  config.withArray("nfcGpioPins", [&](JsonBuilder& arr) {
    for (auto &&pin : miscConfig.nfcGpioPins) {
      arr.addItemToArray(JsonGuard(cJSON_CreateNumber(pin)));
    }
  });
  
  config.addNumber("nfcReaderType", miscConfig.nfcReaderType);
  config.addNumber("nfcIrqPin", miscConfig.nfcIrqPin);
  config.addNumber("nfcVenPin", miscConfig.nfcVenPin);
  config.addBool("ethernetEnabled", miscConfig.ethernetEnabled);
  config.addNumber("ethActivePreset", miscConfig.ethActivePreset);
  config.addNumber("ethPhyType", miscConfig.ethPhyType);
  config.addNumber("ethSpiBus", miscConfig.ethSpiBus);

  config.withArray("ethRmiiConfig", [&](JsonBuilder& arr) {
    for (auto &&val : miscConfig.ethRmiiConfig) {
      arr.addItemToArray(JsonGuard(cJSON_CreateNumber(val)));
    }
  });

  config.withArray("ethSpiConfig", [&](JsonBuilder& arr) {
    for (auto &&val : miscConfig.ethSpiConfig) {
      arr.addItemToArray(JsonGuard(cJSON_CreateNumber(val)));
    }
  });

  config.addBool("overrideStrappingRestriction", miscConfig.overrideStrappingRestriction);
  config.addBool("nfcFastPollingEnabled", miscConfig.nfcFastPollingEnabled);

  httpd_resp_set_type(req, "application/json");
  std::string response = JsonBuilder::object()
      .addBool("success", true)
      .addItem("data", std::move(config).release())
      .toStringUnformatted();
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static bool connectWiFi(const char* ssid, const char* password, int timeoutMs = 30000) {
  ESP_LOGI("WiFiTest", "Testing connection to SSID: %s", ssid);

  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  
  int elapsed = 0;
  const int checkInterval = 1000;
  bool connected = false;
  
  while (elapsed < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(checkInterval));
    elapsed += checkInterval;
    
    if (WiFi.isConnected()) {
      ESP_LOGI("WiFiTest", "Connected to %s, RSSI: %d", ssid, WiFi.RSSI());
      connected = true;
      break;
    } else {
      ESP_LOGW("WiFiTest", "Not yet connected!");
    }
  }
  
  if (!connected) {
    ESP_LOGE("WiFiTest", "Connection timeout after %d ms", timeoutMs);
    WiFi.disconnect();
  }
  
  return connected;
}

struct WifiSaveParams {
    httpd_req_t* req;
    WebServerManager* instance;
    std::string ssid;
    std::string password;
    std::string setupCode;
    bool hasSetupCode;
    std::string cleaned_body_str;
};

struct EthSaveParams {
    httpd_req_t* req;
    WebServerManager* instance;
    std::string setupCode;
    bool hasSetupCode;
    std::string cleaned_body_str;
};

static constexpr int ETH_IP_WAIT_MS = 3000;

/**
 * @brief Persist the captive-portal submission, start the ethernet driver, and
 *        report whether it came up.
 *
 * Runs off the HTTPD task (the request is completed asynchronously). The
 * ETH_GOT_IP subscription is registered before the driver starts so the event
 * cannot be missed. Outcomes:
 * - driver failed to start -> 400, error message; the portal lets the user fix
 *   the ethernet settings and resubmit.
 * - driver started + IP within ETH_IP_WAIT_MS -> success with the real IP.
 * - driver started, no IP -> success with 0.0.0.0 and an explanatory message;
 *   the device reports 0.0.0.0 until the link/DHCP comes up after reboot.
 */
void WebServerManager::captivePortalEthSaveTask(void *pvParameters) {
  EthSaveParams *params = static_cast<EthSaveParams *>(pvParameters);

  if (params->hasSetupCode) {
    homeSpan.setPairingCode(params->setupCode.c_str(), false);
  }

  params->instance->m_configManager.updateFromJson<espConfig::misc_config_t>(
      params->cleaned_body_str);
  params->instance->m_configManager.saveConfig<espConfig::misc_config_t>();

  EventGroupHandle_t ethEvents = xEventGroupCreate();
  auto gotIpSub = AppEventLoop::subscribe(ETH_APP_EVENT, ETH_GOT_IP,
      [ethEvents](const uint8_t* data, size_t size){
        if (ethEvents) xEventGroupSetBits(ethEvents, BIT0);
      });

  const auto miscConfig =
      params->instance->m_configManager.getConfig<espConfig::misc_config_t>();
  const bool driverStarted = EthernetDriver::start(miscConfig);

  bool gotIp = false;
  std::string ipAddr = "0.0.0.0";
  if (driverStarted) {
    if (ethEvents && gotIpSub.is_valid()) {
      gotIp = (xEventGroupWaitBits(ethEvents, BIT0, pdFALSE, pdFALSE,
                                   pdMS_TO_TICKS(ETH_IP_WAIT_MS)) & BIT0) != 0;
    }
    if (gotIp) {
      ipAddr = ETH.localIP().toString().c_str();
    }
  }

  if (gotIpSub.is_valid()) gotIpSub.reset();
  if (ethEvents) vEventGroupDelete(ethEvents);

  httpd_resp_set_type(params->req, "application/json");
  JsonBuilder res = JsonBuilder::object();
  std::string message;
  if (!driverStarted) {
    httpd_resp_set_status(params->req, "400 Bad Request");
    res.addBool("success", false);
    message = "Ethernet driver failed to start. Please check your Ethernet "
              "module settings and try again.";
    res.addString("error", message.c_str());
  } else {
    res.addBool("success", true);
    if (gotIp) {
      message = "Configuration saved. Device will now reboot.";
    } else {
      message = "Configuration saved. The Ethernet driver started, but no IP "
                "address was assigned within 3 seconds. Device will now reboot.";
    }
    res.withObject("data", [&](JsonBuilder &data) {
      data.addString("ip_addr", ipAddr.c_str());
    });
    // The main Web UI needs these credentials, and this is the last screen the user
    // sees before the device reboots, so repeat them in the message.
    const auto &savedMisc = params->instance->m_configManager.getConfig<espConfig::misc_config_t>();
    if (savedMisc.webAuthEnabled) {
      message += fmt::format(" Web UI login: {} / {}", savedMisc.webUsername, savedMisc.webPassword);
    }
    res.addString("message", message.c_str());
  }

  std::string response = res.toStringUnformatted();
  httpd_resp_send(params->req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  httpd_req_async_handler_complete(params->req);
  delete params;
  vTaskDelete(NULL);
}

void WebServerManager::captivePortalSaveTask(void *pvParameters) {
  WifiSaveParams *params = static_cast<WifiSaveParams *>(pvParameters);

  bool connected =
      connectWiFi(params->ssid.c_str(), params->password.c_str(), 15000);

  if (connected) {
    homeSpan.setWifiCredentials(params->ssid.c_str(), params->password.c_str());

    if (params->hasSetupCode) {
      homeSpan.setPairingCode(params->setupCode.c_str(), false);
    }

    params->instance->m_configManager.updateFromJson<espConfig::misc_config_t>(
        params->cleaned_body_str);
    params->instance->m_configManager.saveConfig<espConfig::misc_config_t>();

    std::string ipAddr = WiFi.localIP().toString().c_str();

    httpd_resp_set_type(params->req, "application/json");
    JsonBuilder res = JsonBuilder::object();
    res.addBool("success", true);
    std::string message = "Configuration saved successfully.";
    // The main Web UI needs these credentials, and this is the last screen the user
    // sees before the device reboots, so repeat them in the message.
    const auto &savedMisc = params->instance->m_configManager.getConfig<espConfig::misc_config_t>();
    if (savedMisc.webAuthEnabled) {
      message += fmt::format(" Web UI login: {} / {}", savedMisc.webUsername, savedMisc.webPassword);
    }
    res.addString("message", message.c_str());
    res.withObject("data", [&](JsonBuilder &data) {
      data.addString("ip_addr", ipAddr.c_str());
    });

    std::string response = res.toStringUnformatted();
    httpd_resp_send(params->req, response.c_str(), HTTPD_RESP_USE_STRLEN);

    httpd_req_async_handler_complete(params->req);

    delete params;
  } else {
    httpd_resp_set_status(params->req, "400 Bad Request");
    httpd_resp_set_type(params->req, "application/json");
    std::string response =
        JsonBuilder::object()
            .addBool("success", false)
            .addString("error", "Failed to connect to WiFi network. Please "
                                "check your credentials and try again.")
            .toStringUnformatted();
    httpd_resp_send(params->req, response.c_str(), HTTPD_RESP_USE_STRLEN);

    httpd_req_async_handler_complete(params->req);

    delete params;
  }
  vTaskDelete(NULL);
}

esp_err_t WebServerManager::handleSaveCaptivePortalConfig(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  if (!instance->basicAuth(req)) {
    return sendAuthFailure(req);
  }

  const size_t max_content_size = 2048;
  if (req->content_len >= max_content_size) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_set_type(req, "application/json");
    std::string response = JsonBuilder::object()
        .addBool("success", false)
        .addString("error", "Request body too large")
        .toStringUnformatted();
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  std::vector<char> content(max_content_size, 0);
  int ret = httpd_req_recv(req, content.data(), content.size() - 1);
  if (ret <= 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    std::string response = JsonBuilder::object()
        .addBool("success", false)
        .addString("error", "Invalid request body")
        .toStringUnformatted();
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }
  content[ret] = '\0';

  JsonGuard obj(cJSON_Parse(content.data()));
  if (!obj) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    std::string response = JsonBuilder::object()
        .addBool("success", false)
        .addString("error", "Invalid JSON")
        .toStringUnformatted();
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  std::string ssid;
  std::string password;
  std::string setupCode;
  bool wifiProvided = false;
  bool hasSetupCode = false;

  cJSON *ssidItem = cJSON_GetObjectItem(obj.get(), "wifiSsid");
  cJSON *passwordItem = cJSON_GetObjectItem(obj.get(), "wifiPassword");
  if (ssidItem && cJSON_IsString(ssidItem)) {
    ssid = ssidItem->valuestring;
    if (!ssid.empty()) wifiProvided = true;
  }
  if (passwordItem && cJSON_IsString(passwordItem)) {
    password = passwordItem->valuestring;
  }

  cJSON_DeleteItemFromObject(obj.get(), "wifiSsid");
  cJSON_DeleteItemFromObject(obj.get(), "wifiPassword");

  // Web UI credentials are optional in the setup portal: an empty field means "keep
  // what is stored" and the key is dropped before the body reaches ConfigManager. A
  // device that has not been through first-run setup has no usable password yet, and
  // the check below stops authentication being switched on without one.
  for (const char *key : {"webUsername", "webPassword"}) {
    cJSON *item = cJSON_GetObjectItem(obj.get(), key);
    if (item && cJSON_IsString(item) && item->valuestring[0] == '\0') {
      cJSON_DeleteItemFromObject(obj.get(), key);
    }
  }

  // Turning authentication on without a usable password would leave the Web UI
  // either open or protected by the shipped placeholder value.
  cJSON *webAuthItem = cJSON_GetObjectItem(obj.get(), "webAuthEnabled");
  if (webAuthItem && cJSON_IsBool(webAuthItem) && cJSON_IsTrue(webAuthItem)) {
    cJSON *webPasswordItem = cJSON_GetObjectItem(obj.get(), "webPassword");
    const auto &curMisc = instance->m_configManager.getConfig<espConfig::misc_config_t>();
    const std::string effectivePassword =
        (webPasswordItem && cJSON_IsString(webPasswordItem)) ? webPasswordItem->valuestring
                                                            : curMisc.webPassword;
    if (effectivePassword.empty() || effectivePassword == WEB_AUTH_PASSWORD) {
      return sendJsonError(req, "Set a Web UI password to enable Web UI authentication");
    }
  }

  cJSON *ethEnabledItem = cJSON_GetObjectItem(obj.get(), "ethernetEnabled");
  bool ethernetEnabled = (ethEnabledItem && cJSON_IsBool(ethEnabledItem) && cJSON_IsTrue(ethEnabledItem));

  if (!ethernetEnabled && !wifiProvided) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    std::string response = JsonBuilder::object()
        .addBool("success", false)
        .addString("error", "WiFi SSID and password are required (or enable Ethernet)")
        .toStringUnformatted();
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  if (wifiProvided) {
    if (ssid.length() > 32 || password.length() < 8 || password.length() > 64) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_set_type(req, "application/json");
      std::string response = JsonBuilder::object()
          .addBool("success", false)
          .addString("error", "Invalid WiFi credentials length")
          .toStringUnformatted();
      httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
      return ESP_FAIL;
    }
  }

  cJSON *colorItem = cJSON_GetObjectItem(obj.get(), "hk_key_color");
  if (colorItem && cJSON_IsNumber(colorItem) && colorItem->valueint > 3) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid hk_key_color (must be <= 3)\"}");
    return ESP_FAIL;
  }

  cJSON *nfcReaderTypeItem = cJSON_GetObjectItem(obj.get(), "nfcReaderType");
  if (nfcReaderTypeItem && cJSON_IsNumber(nfcReaderTypeItem) && (nfcReaderTypeItem->valueint < 0 || nfcReaderTypeItem->valueint > 2)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid nfcReaderType\"}");
    return ESP_FAIL;
  }

  cJSON *setupCodeItem = cJSON_GetObjectItem(obj.get(), "setupCode");
  if (setupCodeItem && cJSON_IsString(setupCodeItem)) {
    setupCode = setupCodeItem->valuestring;
    hasSetupCode = true;
  }

  std::string currentConfigJson = instance->m_configManager.serializeToJson<espConfig::misc_config_t>();
  JsonGuard currentConfigData(cJSON_Parse(currentConfigJson.c_str()));
  if (!currentConfigData) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  bool isValid = instance->validateRequest(req, currentConfigData.get(), obj.get());
  if (!isValid) {
    return ESP_FAIL; 
  }
  
  std::string cleaned_body_str = to_string_unformatted(obj);

  if (wifiProvided) {
    httpd_req_t* reqCopy = nullptr;
    if (httpd_req_async_handler_begin(req, &reqCopy) != ESP_OK) {
      return sendJsonError(req, "Failed to start save operation");
    }

    WifiSaveParams* params = new WifiSaveParams{
      .req = reqCopy,
      .instance = instance,
      .ssid = ssid,
      .password = password,
      .setupCode = setupCode,
      .hasSetupCode = hasSetupCode,
      .cleaned_body_str = cleaned_body_str
    };

BaseType_t task;
#ifndef CONFIG_FREERTOS_UNICORE
    task = xTaskCreatePinnedToCore(captivePortalSaveTask, "wifi_save_task", 8192, params, 5, nullptr, 1);
#else
    task = xTaskCreate(captivePortalSaveTask, "wifi_save_task", 8192, params, 5, nullptr);
#endif
    if (task != pdPASS) {
      ESP_LOGE(TAG, "Failed to create WiFi save task");
      delete params;
      httpd_req_async_handler_complete(reqCopy);
      return sendJsonError(req, "Failed to create save task");
    }

    return ESP_OK;
  }

  if (ethernetEnabled) {
    httpd_req_t* reqCopy = nullptr;
    if (httpd_req_async_handler_begin(req, &reqCopy) != ESP_OK) {
      return sendJsonError(req, "Failed to start save operation");
    }

    EthSaveParams* params = new EthSaveParams{
      .req = reqCopy,
      .instance = instance,
      .setupCode = setupCode,
      .hasSetupCode = hasSetupCode,
      .cleaned_body_str = cleaned_body_str
    };

    BaseType_t task;
#ifndef CONFIG_FREERTOS_UNICORE
    task = xTaskCreatePinnedToCore(captivePortalEthSaveTask, "eth_save_task", 8192, params, 5, nullptr, 1);
#else
    task = xTaskCreate(captivePortalEthSaveTask, "eth_save_task", 8192, params, 5, nullptr);
#endif
    if (task != pdPASS) {
      ESP_LOGE(TAG, "Failed to create Ethernet save task");
      delete params;
      httpd_req_async_handler_complete(reqCopy);
      return sendJsonError(req, "Failed to create save task");
    }

    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  std::string response = JsonBuilder::object()
      .addBool("success", true)
      .addString("message", "Configuration saved successfully")
      .toStringUnformatted();
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t WebServerManager::handleWifiScan(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance || !instance->basicAuth(req)) {
    return sendAuthFailure(req);
  }
  ESP_LOGI(TAG, "Starting WiFi scan...");

  wifi_mode_t current_mode;
  esp_wifi_get_mode(&current_mode);

  bool need_restore_mode = false;
  if (current_mode == WIFI_MODE_AP) {
    ESP_LOGI(TAG, "Temporarily enabling APSTA mode for scanning");
    esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(mode_err));
      httpd_resp_set_status(req, "500 Internal Server Error");
      httpd_resp_set_type(req, "application/json");
      std::string response = JsonBuilder::object()
          .addBool("success", false)
          .addString("error", "Failed to enable scan mode")
          .toStringUnformatted();
      httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
    need_restore_mode = true;
  }

  wifi_scan_config_t scan_config = {};
  scan_config.channel = 0;
  scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_config.scan_time.active.min = 100;
  scan_config.scan_time.active.max = 300;

  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi scan failed to start: %s", esp_err_to_name(err));
    if (need_restore_mode) {
      esp_wifi_set_mode(current_mode);
    }
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    std::string response = JsonBuilder::object()
        .addBool("success", false)
        .addString("error", "WiFi scan failed to start")
        .toStringUnformatted();
    httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  constexpr uint16_t MAX_AP_COUNT = 20;
  if (ap_count > MAX_AP_COUNT) {
    ESP_LOGW(TAG, "Capping AP scan results from %d to %d", ap_count, MAX_AP_COUNT);
    ap_count = MAX_AP_COUNT;
  }
  ESP_LOGI(TAG, "Found %d access points", ap_count);

  wifi_ap_record_t ap_records[MAX_AP_COUNT];
  esp_wifi_scan_get_ap_records(&ap_count, ap_records);

  if (need_restore_mode) {
    esp_wifi_set_mode(current_mode);
    ESP_LOGI(TAG, "Restored WiFi mode to AP");
  }

  JsonBuilder res = JsonBuilder::object();
  res.addBool("success", true);
  res.withArray("data", [&](JsonBuilder& networks) {
    for (int i = 0; i < ap_count; i++) {
      JsonBuilder network = JsonBuilder::object();
      network.addString("ssid", (char*)ap_records[i].ssid);
      network.addNumber("rssi", ap_records[i].rssi);
      network.addNumber("channel", ap_records[i].primary);

      const char* auth_mode;
      switch (ap_records[i].authmode) {
        case WIFI_AUTH_OPEN: auth_mode = "OPEN"; break;
        case WIFI_AUTH_WEP: auth_mode = "WEP"; break;
        case WIFI_AUTH_WPA_PSK: auth_mode = "WPA_PSK"; break;
        case WIFI_AUTH_WPA2_PSK: auth_mode = "WPA2_PSK"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: auth_mode = "WPA_WPA2_PSK"; break;
        case WIFI_AUTH_WPA2_ENTERPRISE: auth_mode = "WPA2_ENTERPRISE"; break;
        case WIFI_AUTH_WPA3_PSK: auth_mode = "WPA3_PSK"; break;
        case WIFI_AUTH_WPA2_WPA3_PSK: auth_mode = "WPA2_WPA3_PSK"; break;
        default: auth_mode = "UNKNOWN"; break;
      }
      network.addString("auth", auth_mode);
      networks.addItemToArray(std::move(network).release());
    }
  });
  res.addString("message", "WiFi scan complete");

  httpd_resp_set_type(req, "application/json");
  std::string response = res.toStringUnformatted();
  httpd_resp_send(req, response.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// ============================================================================
// WebSocket Implementation
// ============================================================================

struct AsyncWsData {
  httpd_handle_t server;
  int fd;
  httpd_ws_type_t type;
  std::vector<uint8_t> payload;
};

esp_err_t WebServerManager::handleWebSocket(httpd_req_t *req) {
#ifndef CONFIG_HTTPD_WS_SUPPORT
  httpd_resp_set_status(req, "501 Not Implemented");
  httpd_resp_send(req, "WebSocket not enabled", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
#else
  WebServerManager *instance = getInstance(req);
  if (!instance)
    return ESP_FAIL;

  if (req->method == HTTP_GET) {
    char *sessionId = new char[65];
    size_t sessionIdLen = 65;
    esp_err_t err = httpd_req_get_cookie_val(req, "sessionId", sessionId, &sessionIdLen);
    if(!instance->basicAuth(req) && (err != ESP_OK || strncmp(sessionId, instance->m_sessionId.c_str(), sessionIdLen) != 0)){
      delete[] sessionId;
      return sendAuthFailure(req);
    }
    delete[] sessionId;

    // Handshake check succeeded. Returning ESP_OK completes the handshake.
    // The server will invoke WebServerManager::ws_post_handshake_cb immediately after.
    return ESP_OK;
  }

  // Receive WebSocket frame
  httpd_ws_frame_t ws_pkt = {};
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);

  if (ret != ESP_OK) {
    int sockfd = httpd_req_to_sockfd(req);
    const char *err_name = esp_err_to_name(ret);
    bool is_protocol_error =
        (err_name &&
         (strstr(err_name, "masked") || strstr(err_name, "MASKED"))) ||
        errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN;
    if (ret == ESP_FAIL || ret == ESP_ERR_INVALID_STATE ||
        ret == ESP_ERR_INVALID_ARG || is_protocol_error) {
      instance->removeWebSocketClient(sockfd);
    }
    return ret;
  }

  if (ws_pkt.len > MAX_WS_PAYLOAD) {
    ESP_LOGE(TAG, "Payload too large: %zu", ws_pkt.len);
    instance->removeWebSocketClient(httpd_req_to_sockfd(req));
    return ESP_FAIL;
  }

  std::string payload;
  if (ws_pkt.len) {
    payload.resize(ws_pkt.len);
    ws_pkt.payload = (uint8_t *)payload.data();
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
      int sockfd = httpd_req_to_sockfd(req);
      const char *err_name = esp_err_to_name(ret);
      bool is_protocol_error =
          (err_name &&
           (strstr(err_name, "masked") || strstr(err_name, "MASKED"))) ||
          errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN;
      if (ret == ESP_FAIL || ret == ESP_ERR_INVALID_STATE ||
          ret == ESP_ERR_INVALID_ARG || is_protocol_error) {
        instance->removeWebSocketClient(sockfd);
      }
      return ret;
    }
  }

  if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
    return instance->handleWebSocketMessage(req, payload);
  } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
    instance->removeWebSocketClient(httpd_req_to_sockfd(req));
  }
  return ESP_OK;
#endif
}

void WebServerManager::addWebSocketClient(int fd) {
  std::scoped_lock lock(m_wsClientsMutex);
  auto it = std::find_if(
      m_wsClients.begin(), m_wsClients.end(),
      [fd](const std::unique_ptr<WsClient> &c) { return c->fd == fd; });
  if (it == m_wsClients.end()) {
    m_wsClients.emplace_back(std::make_unique<WsClient>(fd));
  }
}

void WebServerManager::removeWebSocketClient(int fd) {
  bool stopTimer = false;
  size_t remaining = 0;
  bool removed = false;
  {
    std::scoped_lock lock(m_wsClientsMutex);
    auto it = std::find_if(
        m_wsClients.begin(), m_wsClients.end(),
        [fd](const std::unique_ptr<WsClient> &c) { return c->fd == fd; });
    if (it != m_wsClients.end()) {
      m_wsClients.erase(it);
      remaining = m_wsClients.size();
      stopTimer = m_wsClients.empty();
      removed = true;
    }
  }
  if (removed) {
    ESP_LOGI(TAG, "Removed WebSocket client fd=%d, remaining: %zu", fd,
             remaining);
  }
  if (stopTimer && m_statusTimer && esp_timer_is_active(m_statusTimer)) {
    esp_timer_stop(m_statusTimer);
  }
}

void WebServerManager::setWSBackLogSize(const uint16_t size){
  wsBacklogSize = size;
}

void WebServerManager::broadcastWs(const uint8_t *payload, size_t len,
                                   httpd_ws_type_t type) {
  std::vector<int> fds;
  {
    std::scoped_lock lock(m_wsClientsMutex); 
    fds.reserve(m_wsClients.size());
    for (const auto &c : m_wsClients)
      fds.push_back(c->fd);
  }
  if (fds.empty() && wsBacklogSize > 0) {
    if(m_wsBroadcastBuffer.size() >= wsBacklogSize){
      m_wsBroadcastBuffer.pop_front();
    }
    m_wsBroadcastBuffer.emplace_back(payload, payload + len);
    return;
  }
  for (int fd : fds){
    queue_ws_frame(fd, payload, len, type);
  }
}

void WebServerManager::queue_ws_frame(int fd, const uint8_t *payload,
                                      size_t len, httpd_ws_type_t type) {
  WsFrame *frame = new WsFrame;
  if (!frame)
    return;

  frame->fd = fd;
  frame->type = type;
  frame->len = len;
  if (len <= WsFrame::INLINE_SIZE) {
    memcpy(frame->inlinePayload, payload, len);
    frame->payload = frame->inlinePayload;
  } else {
    frame->payload = new uint8_t[len];
    memcpy(frame->payload, payload, len);
  }

  // Never block the caller (this runs on the log sink task): drop and count
  // instead. A blocked enqueue here would stall all log dispatch.
  if (xQueueSend(m_wsQueue, &frame, 0) != pdTRUE) {
    m_wsFrameDropped.fetch_add(1, std::memory_order_relaxed);
    if (frame->payload != frame->inlinePayload)
      delete[] frame->payload;
    delete frame;
  }
}

void WebServerManager::ws_send_task(void *arg) {
  WebServerManager *instance = static_cast<WebServerManager *>(arg);
  WsFrame *raw_frame = nullptr;

  while (true) {
    if (xQueueReceive(instance->m_wsQueue, &raw_frame, portMAX_DELAY) !=
        pdPASS) {
      continue;
    }
    if (!raw_frame)
      continue;

    // Drain everything already queued before waiting again so a burst of
    // frames costs one wake-up instead of one per frame.
    do {
      WsFramePtr frame(raw_frame);

      int target_fd = -1;
      {
        std::scoped_lock<std::mutex> lock(instance->m_wsClientsMutex);
        auto it = std::find_if(
            instance->m_wsClients.begin(), instance->m_wsClients.end(),
            [fd = frame->fd](const std::unique_ptr<WsClient> &c) {
              return c->fd == fd;
            });
        if (it != instance->m_wsClients.end())
          target_fd = frame->fd;
      }

      if (target_fd != -1) {
        httpd_ws_frame_t ws_pkt = {};
        ws_pkt.final = true;
        ws_pkt.fragmented = false;
        ws_pkt.type = frame->type;
        ws_pkt.len = frame->len;
        ws_pkt.payload = frame->payload;

        esp_err_t send_ret = httpd_ws_send_frame_async(instance->m_server, target_fd, &ws_pkt);
        if (send_ret != ESP_OK) {
          const char *err = esp_err_to_name(send_ret);
          bool is_err =
              (err && (strstr(err, "masked") || strstr(err, "MASKED"))) ||
              send_ret == ESP_FAIL;
          if (is_err || send_ret == ESP_ERR_INVALID_STATE ||
              send_ret == ESP_ERR_INVALID_ARG) {
            instance->removeWebSocketClient(frame->fd);
          }
        }
      }
    } while (xQueueReceive(instance->m_wsQueue, &raw_frame, 0) == pdPASS &&
             raw_frame != nullptr);
  }
}

esp_err_t WebServerManager::handleWebSocketMessage(httpd_req_t *req, const std::string &message) {
  JsonGuard json(cJSON_Parse(message.c_str()));
  if (!json) {
    std::string err_str = JsonBuilder::object()
        .addString("type", "error")
        .addString("message", "Invalid JSON format")
        .toStringUnformatted();
    queue_ws_frame(httpd_req_to_sockfd(req), (const uint8_t *)err_str.c_str(), err_str.size(), HTTPD_WS_TYPE_TEXT);
    return ESP_OK;
  }

  cJSON *type_item = cJSON_GetObjectItem(json.get(), "type");
  if (!type_item || !cJSON_IsString(type_item)) {
    return ESP_OK;
  }

  std::string msg_type = type_item->valuestring;
  int sockfd = httpd_req_to_sockfd(req);
  std::string response;

  if (msg_type == "ping") {
    response = JsonBuilder::object()
        .addString("type", "pong")
        .addNumber("timestamp", static_cast<uint32_t>(esp_timer_get_time() / 1000))
        .toStringUnformatted();
  } else if (msg_type == "metrics") {
    response = getDeviceMetrics();
  } else if (msg_type == "sysinfo") {
    response = getDeviceInfo();
  } else if (msg_type == "ota_info") {
    response = getOTAInfo();
  } else if (msg_type == "set_log_level") {  
    cJSON *level_item = cJSON_GetObjectItem(json.get(), "data");
    if(level_item && cJSON_IsNumber(level_item)) {
      esp_log_level_t level = esp_log_level_t(level_item->valueint >= 0 && level_item->valueint < 6 ? level_item->valueint : ESP_LOG_WARN);
      esp_log_level_set("*", level);
      loggable::Sinker::instance().set_level(level_item->valueint >= 0 && level_item->valueint < 6 ? (loggable::LogLevel)level_item->valueint : loggable::LogLevel::Warning);
      m_configManager.setNVSLogLevel(level);
    }
    response = getDeviceInfo();
  } else if (msg_type == "set_backlog_max_size") {
    cJSON *item = cJSON_GetObjectItem(json.get(), "data");
    if(item && cJSON_IsNumber(item)) {
      if(item->valueint >= 0 && item->valueint <= 65535){
        wsBacklogSize = item->valueint;
        m_configManager.setBacklogMaxSize(item->valueint);
      } else ESP_LOGE(TAG, "Number outside of range for 'set_backlog_max_size'");
    }
    response = getDeviceInfo();
  } else {
    response = JsonBuilder::object()
        .addString("type", "error")
        .addString("message", "Unknown message type")
        .addString("received_type", msg_type.c_str())
        .toStringUnformatted();
  }

  queue_ws_frame(sockfd, (const uint8_t *)response.c_str(), response.size(), HTTPD_WS_TYPE_TEXT);
  return ESP_OK;
}

// ============================================================================
// Device Info/Status Methods
// ============================================================================

std::string WebServerManager::getDeviceMetrics() {
  JsonBuilder status = JsonBuilder::object();
  status.addString("type", "metrics");
  status.addNumber("uptime", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<int64_t, std::micro>(esp_timer_get_time())).count());
  status.addNumber("free_heap", esp_get_free_heap_size());
  status.addNumber("wifi_rssi", WiFi.RSSI());
  status.addBool("nfc_connected", m_nfcManager ? m_nfcManager->isConnected() : false);
  status.addNumber("nfc_reader_type", m_configManager.getConfig<espConfig::misc_config_t>().nfcReaderType);
  status.addBool("mqtt_connected", m_mqttManager ? m_mqttManager->isConnected() : false);
  status.addNumber("mqtt_error_code", m_mqttManager ? static_cast<uint8_t>(m_mqttManager->getLastErrorCode()) : 0);
  status.addNumber("ws_frames_dropped", static_cast<uint64_t>(getWsFrameDropCount()));
  if (m_mqttManager && !m_mqttManager->getLastErrorMessage().empty()) {
    status.addString("mqtt_error_message", m_mqttManager->getLastErrorMessage().c_str());
  }
  return status.toStringUnformatted();
}

std::string WebServerManager::getDeviceInfo() {
  JsonBuilder info = JsonBuilder::object();
  info.addString("type", "sysinfo");
  info.addString("deviceName", m_configManager.getConfig<espConfig::misc_config_t>().deviceName.c_str());
  info.addString("version", esp_app_get_description()->version);
  info.addBool("eth_enabled", m_configManager.getConfig<espConfig::misc_config_t>().ethernetEnabled);
  info.addString("wifi_ssid", WiFi.SSID().c_str());
  info.addNumber("log_level", esp_log_level_get("*"));
  esp_chip_info_t chipInfo;
  esp_chip_info(&chipInfo);
  info.addNumber("chip_model", chipInfo.model);
  info.addNumber("backlog_max_size", wsBacklogSize);
  return info.toStringUnformatted();
}

void WebServerManager::statusTimerCallback(void *arg) {
  WebServerManager *instance = static_cast<WebServerManager *>(arg);
  auto metrics = instance->getDeviceMetrics();
  instance->broadcastWs((const uint8_t *)(metrics.c_str()), metrics.size(),
                        HTTPD_WS_TYPE_TEXT);
}

// ============================================================================
// OTA Implementation
// ============================================================================

// ============================================================================
// GitHub release updater
// ============================================================================
//
// Pulls a published release straight from GitHub instead of asking the user to
// download a .bin and upload it through the browser. Two channels:
//
//   * production  - GET /releases/latest, which GitHub defines as the newest release
//                   that is neither a draft nor a pre-release.
//   * development - the newest entry of GET /releases, which is where pre-releases
//                   appear. /releases/latest silently skips them, so the development
//                   channel cannot be built on it.
//
// Only the assets the release workflow uploads are used: `esp32.firmware.bin` (the OTA
// application image) and `littlefs.bin` (the Web UI filesystem). Both are installed
// together, because the UI lives in the filesystem image and a firmware-only update can
// leave the two out of step.
//
// SECURITY NOTE: while the device runs in Path 1 (see
// docs/content/PATH2_SECURITY_ROLLOUT.md) Secure Boot is off, so nothing here is
// signature-checked - only the ESP image header and checksum that esp_ota_end()
// validates, over a TLS connection whose certificate is verified against the CA bundle.
// Enabling Secure Boot is what turns "matches what GitHub served" into "is what the
// maintainer signed".

namespace {

// Point these at a different fork to change where updates come from.
constexpr const char *kGithubOwner = "CsepregiArtur";
constexpr const char *kGithubRepo = "HomeKey-ESP32";

constexpr const char *kFirmwareAsset = "esp32.firmware.bin";
constexpr const char *kFilesystemAsset = "littlefs.bin";

/// Upper bound on an API response body, to avoid exhausting the heap on a bad reply.
constexpr size_t kMaxJsonBody = 256 * 1024;

struct ReleaseInfo {
  std::string error;
  std::string tag;
  std::string name;
  std::string publishedAt;
  bool prerelease = false;
  std::string firmwareUrl;
  std::string filesystemUrl;
  size_t firmwareSize = 0;
  size_t filesystemSize = 0;
};

using ProgressFn = std::function<void(size_t, size_t)>;

/// GET a URL into `out`. Only for the small JSON API calls, never for the images.
bool httpGetToString(const std::string &url, std::string &out, std::string &err) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 20000;
  cfg.buffer_size = 2048;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    err = "Could not create the HTTP client";
    return false;
  }
  // GitHub rejects API requests that do not identify themselves.
  esp_http_client_set_header(client, "User-Agent", "HomeKey-ESP32");
  esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

  // A TLS client connection needs tens of KB, and it is attempted while the HTTPS server
  // may be holding a TLS context per socket and HomeKit/MQTT are running. Checked up front
  // so the likely cause is named instead of surfacing as a generic connect failure.
  const size_t freeBefore = esp_get_free_heap_size();
  if (freeBefore < HEAP_LOWER_THRESHOLD) {
    err = fmt::format("Not enough free memory ({} bytes) to open a TLS connection",
                      freeBefore);
    esp_http_client_cleanup(client);
    return false;
  }

  const esp_err_t openErr = esp_http_client_open(client, 0);
  if (openErr != ESP_OK) {
    // Include the actual reason and the heap at the time: without them this is
    // indistinguishable between "no internet", "DNS failed", "TLS refused" and "ran out
    // of memory", which are four very different problems.
    err = fmt::format("Could not reach GitHub: {} (free memory {} bytes)",
                      esp_err_to_name(openErr), esp_get_free_heap_size());
    esp_http_client_cleanup(client);
    return false;
  }
  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    // 404 from /releases/latest means "no stable release published yet".
    err = (status == 404) ? "No matching release found"
                          : fmt::format("GitHub returned HTTP {}", status);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  std::vector<char> buf(1024);
  out.clear();
  int read = 0;
  while ((read = esp_http_client_read(client, buf.data(), buf.size())) > 0) {
    out.append(buf.data(), read);
    if (out.size() > kMaxJsonBody) {
      err = "GitHub response was unexpectedly large";
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (read < 0) {
    err = "The connection to GitHub was interrupted";
    return false;
  }
  return true;
}

/// Pick the two assets we care about out of a release object.
void collectAssets(cJSON *release, ReleaseInfo &info) {
  cJSON *assets = cJSON_GetObjectItemCaseSensitive(release, "assets");
  if (!cJSON_IsArray(assets)) {
    return;
  }
  for (cJSON *asset = assets->child; asset != nullptr; asset = asset->next) {
    cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
    cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
    if (!cJSON_IsString(name) || !cJSON_IsString(url)) {
      continue;
    }
    const std::string assetName = name->valuestring;
    const size_t assetSize = cJSON_IsNumber(size) ? static_cast<size_t>(size->valuedouble) : 0;
    if (assetName == kFirmwareAsset) {
      info.firmwareUrl = url->valuestring;
      info.firmwareSize = assetSize;
    } else if (assetName == kFilesystemAsset) {
      info.filesystemUrl = url->valuestring;
      info.filesystemSize = assetSize;
    }
  }
}

/// Resolve the release for the requested channel.
bool resolveRelease(bool developmentChannel, ReleaseInfo &info) {
  const std::string url = developmentChannel
      ? fmt::format("https://api.github.com/repos/{}/{}/releases", kGithubOwner, kGithubRepo)
      : fmt::format("https://api.github.com/repos/{}/{}/releases/latest", kGithubOwner, kGithubRepo);

  std::string body;
  if (!httpGetToString(url, body, info.error)) {
    return false;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    info.error = "Could not parse the GitHub response";
    return false;
  }

  // /releases returns an array (newest first); /releases/latest returns one object.
  cJSON *release = root;
  if (cJSON_IsArray(root)) {
    release = cJSON_GetArrayItem(root, 0);
  }
  if (!cJSON_IsObject(release)) {
    info.error = developmentChannel ? "No releases found" : "No stable release published yet";
    cJSON_Delete(root);
    return false;
  }

  auto stringField = [release](const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(release, key);
    return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
  };
  info.tag = stringField("tag_name");
  info.name = stringField("name");
  info.publishedAt = stringField("published_at");
  info.prerelease = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "prerelease"));
  collectAssets(release, info);
  cJSON_Delete(root);

  if (info.firmwareUrl.empty()) {
    info.error = fmt::format("Release {} does not contain {}", info.tag, kFirmwareAsset);
    return false;
  }
  if (info.filesystemUrl.empty()) {
    info.error = fmt::format("Release {} does not contain {}", info.tag, kFilesystemAsset);
    return false;
  }
  return true;
}

/// Open a streaming GET for a download URL and report the expected body size.
bool openAssetStream(const std::string &url, esp_http_client_handle_t &client, std::string &err) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 30000;
  cfg.buffer_size = 4096;

  client = esp_http_client_init(&cfg);
  if (!client) {
    err = "Could not create the HTTP client";
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", "HomeKey-ESP32");

  if (esp_http_client_open(client, 0) != ESP_OK) {
    err = "Could not start the download";
    esp_http_client_cleanup(client);
    client = nullptr;
    return false;
  }
  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    err = fmt::format("The download failed with HTTP {}", status);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = nullptr;
    return false;
  }
  return true;
}

/// Download an application image and stage it as the next boot partition.
bool streamIntoOta(const std::string &url, size_t expectedSize,
                   const esp_partition_t *partition, const ProgressFn &onProgress,
                   std::string &err) {
  esp_http_client_handle_t client = nullptr;
  if (!openAssetStream(url, client, err)) {
    return false;
  }

  const int reported = static_cast<int>(esp_http_client_get_content_length(client));
  const size_t total = reported > 0 ? static_cast<size_t>(reported) : expectedSize;
  if (total == 0 || total > partition->size) {
    err = "The image does not fit the OTA partition";
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  esp_ota_handle_t handle = 0;
  if (esp_ota_begin(partition, total, &handle) != ESP_OK) {
    err = "Could not start writing the firmware";
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  std::vector<char> buf(4096);
  size_t written = 0;
  bool failed = false;
  int read = 0;
  while ((read = esp_http_client_read(client, buf.data(), buf.size())) > 0) {
    if (esp_ota_write(handle, buf.data(), read) != ESP_OK) {
      err = "Writing the firmware failed";
      failed = true;
      break;
    }
    written += static_cast<size_t>(read);
    if (onProgress) {
      onProgress(written, total);
    }
    // Yield so the idle task, the WebSocket sender and the watchdog keep running
    // during what can be a multi-megabyte download.
    vTaskDelay(1);
  }
  if (!failed && read < 0) {
    err = "The firmware download was interrupted";
    failed = true;
  }
  if (!failed && written == 0) {
    err = "The firmware download was empty";
    failed = true;
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (failed) {
    esp_ota_abort(handle);
    return false;
  }
  if (esp_ota_end(handle) != ESP_OK) {
    // esp_ota_end() validates the image header and checksum; it consumes the handle.
    err = "The downloaded firmware image is not valid";
    return false;
  }
  if (esp_ota_set_boot_partition(partition) != ESP_OK) {
    err = "Could not mark the new firmware as the boot image";
    return false;
  }
  return true;
}

/// Download a filesystem image into the LittleFS partition.
bool streamIntoFilesystem(const std::string &url, size_t expectedSize,
                          const esp_partition_t *partition, const ProgressFn &onProgress,
                          std::string &err) {
  esp_http_client_handle_t client = nullptr;
  if (!openAssetStream(url, client, err)) {
    return false;
  }

  const int reported = static_cast<int>(esp_http_client_get_content_length(client));
  const size_t total = reported > 0 ? static_cast<size_t>(reported) : expectedSize;
  if (total == 0 || total > partition->size) {
    err = "The filesystem image does not fit its partition";
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  // Unmount before touching the partition, and remount afterwards.
  LittleFS.end();
  if (esp_partition_erase_range(partition, 0, partition->size) != ESP_OK) {
    err = "Could not erase the filesystem partition";
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  std::vector<char> buf(4096);
  size_t written = 0;
  bool failed = false;
  int read = 0;
  while ((read = esp_http_client_read(client, buf.data(), buf.size())) > 0) {
    if (esp_partition_write(partition, written, buf.data(), read) != ESP_OK) {
      err = "Writing the filesystem failed";
      failed = true;
      break;
    }
    written += static_cast<size_t>(read);
    if (onProgress) {
      onProgress(written, total);
    }
    vTaskDelay(1);
  }
  if (!failed && read < 0) {
    err = "The filesystem download was interrupted";
    failed = true;
  }
  if (!failed && written == 0) {
    err = "The filesystem download was empty";
    failed = true;
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (failed) {
    return false;
  }
  if (!LittleFS.begin()) {
    err = "The new filesystem could not be mounted";
    return false;
  }
  return true;
}

} // namespace

esp_err_t WebServerManager::handleGetReleaseInfo(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance || !instance->basicAuth(req)) {
    return sendAuthFailure(req);
  }

  bool developmentChannel = false;
  char query[128];
  char value[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "channel", value, sizeof(value)) == ESP_OK) {
    developmentChannel = (strcmp(value, "dev") == 0);
  }

  ReleaseInfo release;
  if (!resolveRelease(developmentChannel, release)) {
    return sendJsonError(req, release.error.empty() ? "Could not reach GitHub" : release.error);
  }

  JsonBuilder res = JsonBuilder::object();
  res.addBool("success", true);
  res.withObject("data", [&](JsonBuilder &data) {
    data.addString("channel", developmentChannel ? "dev" : "stable");
    data.addString("tag", release.tag.c_str());
    data.addString("name", release.name.c_str());
    data.addString("published_at", release.publishedAt.c_str());
    data.addBool("prerelease", release.prerelease);
    data.addString("current_version", esp_app_get_description()->version);
    data.withObject("firmware", [&](JsonBuilder &asset) {
      asset.addString("name", kFirmwareAsset);
      asset.addNumber("size", static_cast<double>(release.firmwareSize));
    });
    data.withObject("filesystem", [&](JsonBuilder &asset) {
      asset.addString("name", kFilesystemAsset);
      asset.addNumber("size", static_cast<double>(release.filesystemSize));
    });
  });

  httpd_resp_set_type(req, "application/json");
  const std::string out = res.toStringUnformatted();
  httpd_resp_send(req, out.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t WebServerManager::handleInstallRelease(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance || !instance->basicAuth(req)) {
    return sendAuthFailure(req);
  }

  bool expected = false;
  if (!instance->m_otaInProgress.compare_exchange_strong(expected, true)) {
    return sendJsonError(req, "An update is already in progress", "409 Conflict");
  }

  bool developmentChannel = false;
  if (req->content_len > 0 && req->content_len < 512) {
    std::vector<char> body(req->content_len + 1, '\0');
    const int received = httpd_req_recv(req, body.data(), req->content_len);
    if (received > 0) {
      cJSON *root = cJSON_Parse(body.data());
      if (root != nullptr) {
        cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
        developmentChannel = cJSON_IsString(channel) && strcmp(channel->valuestring, "dev") == 0;
        cJSON_Delete(root);
      }
    }
  }

  auto *params = new GithubOtaParams{instance, developmentChannel, new OTAState()};
  BaseType_t task;
#ifndef CONFIG_FREERTOS_UNICORE
  task = xTaskCreatePinnedToCore(githubOtaTask, "gh_ota_task", 8192, params, 5, nullptr, 1);
#else
  task = xTaskCreate(githubOtaTask, "gh_ota_task", 8192, params, 5, nullptr);
#endif
  if (task != pdPASS) {
    ESP_LOGE(TAG, "Failed to create the GitHub update task");
    delete params->state;
    delete params;
    instance->m_otaInProgress = false;
    return sendJsonError(req, "Could not start the update", "500 Internal Server Error");
  }

  JsonBuilder res = JsonBuilder::object();
  res.addBool("success", true);
  res.addString("message",
                "Downloading the update from GitHub. The device reboots when it finishes.");
  httpd_resp_set_type(req, "application/json");
  const std::string out = res.toStringUnformatted();
  httpd_resp_send(req, out.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

void WebServerManager::githubOtaTask(void *pvParameters) {
  GithubOtaParams *params = static_cast<GithubOtaParams *>(pvParameters);
  WebServerManager *instance = params->instance;
  OTAState *state = params->state;

  state->inProgress = true;
  state->writtenBytes = 0;
  state->totalBytes = 0;
  state->error.clear();
  state->currentUploadType = OTAUploadType::FIRMWARE;
  instance->broadcastOTAStatus(*state);

  bool succeeded = false;
  do {
    ReleaseInfo release;
    if (!resolveRelease(params->developmentChannel, release)) {
      state->error = release.error.empty() ? "Could not resolve the release" : release.error;
      break;
    }
    ESP_LOGI(TAG, "Updating from GitHub release %s (%s)", release.tag.c_str(),
             release.prerelease ? "pre-release" : "stable");

    // Firmware first. If the filesystem landed but the firmware did not, the web UI
    // would be newer than the backend that serves it.
    state->currentUploadType = OTAUploadType::FIRMWARE;
    state->writtenBytes = 0;
    state->totalBytes = release.firmwareSize;
    {
      const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
      if (partition == nullptr) {
        state->error = "No OTA partition available";
        break;
      }
      std::string err;
      const auto progress = [&](size_t written, size_t total) {
        state->writtenBytes = written;
        state->totalBytes = total;
        instance->broadcastOTAStatus(*state);
      };
      if (!streamIntoOta(release.firmwareUrl, release.firmwareSize, partition, progress, err)) {
        state->error = "Firmware: " + err;
        break;
      }
    }

    state->currentUploadType = OTAUploadType::LITTLEFS;
    state->writtenBytes = 0;
    state->totalBytes = release.filesystemSize;
    instance->broadcastOTAStatus(*state);
    {
      const esp_partition_t *partition = esp_partition_find_first(
          ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
      if (partition == nullptr) {
        state->error = "No filesystem partition available";
        break;
      }
      std::string err;
      const auto progress = [&](size_t written, size_t total) {
        state->writtenBytes = written;
        state->totalBytes = total;
        instance->broadcastOTAStatus(*state);
      };
      if (!streamIntoFilesystem(release.filesystemUrl, release.filesystemSize, partition,
                                progress, err)) {
        state->error = "Filesystem: " + err;
        break;
      }
    }

    succeeded = true;
  } while (false);

  state->inProgress = false;
  instance->broadcastOTAStatus(*state);
  instance->m_otaInProgress = false;

  if (!succeeded) {
    ESP_LOGE(TAG, "GitHub update failed: %s", state->error.c_str());
  }

  // Give the WebSocket task time to flush the final status before restarting.
  vTaskDelay(pdMS_TO_TICKS(1500));

  const bool reboot = succeeded;
  delete state;
  delete params;

  if (reboot) {
    ESP_LOGI(TAG, "GitHub update complete; rebooting");
    esp_restart();
  }
  vTaskDelete(nullptr);
}

esp_err_t WebServerManager::handleOTAUpload(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance->basicAuth(req)) {
    return sendAuthFailure(req);
  }
  
  if (req->content_len == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Invalid request\"}");
    return ESP_OK;
  }

  bool expected = false;
  if (!instance->m_otaInProgress.compare_exchange_strong(expected, true)) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"OTA in progress\"}");
    return ESP_OK;
  }

  char *type = strrchr(req->uri, '/');
  OTAUploadType uploadType = (type && strncmp(type + 1, "littlefs", 8) == 0)
                                 ? OTAUploadType::LITTLEFS
                                 : OTAUploadType::FIRMWARE;

 auto app_part =  esp_ota_get_running_partition();
  if (uploadType == OTAUploadType::FIRMWARE && req->content_len > app_part->size) {
    ESP_LOGE(TAG, "OTA size %zu > max %zu", req->content_len, app_part->size);
    instance->m_otaInProgress = false;
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Firmware too large\"}");
    return ESP_OK;
  }
  auto fs_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (uploadType == OTAUploadType::LITTLEFS && req->content_len > fs_part->size) {
    ESP_LOGE(TAG, "OTA size %zu > max %zu", req->content_len, fs_part->size);
    instance->m_otaInProgress = false;
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"LittleFS too large\"}");
    return ESP_OK;
  }

  bool skipReboot = false;
  char query[256], param[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "skipReboot", param, sizeof(param)) ==
          ESP_OK) {
    skipReboot = (strcmp(param, "true") == 0);
  }

  httpd_req_t *reqCopy = nullptr;
  if (httpd_req_async_handler_begin(req, &reqCopy) != ESP_OK) {
    instance->m_otaInProgress = false;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"Failed to start OTA\"}");
    return ESP_OK;
  }

  OTAParams *params = new OTAParams{reqCopy, instance, uploadType, skipReboot, req->content_len, new OTAState()};
  params->state->inProgress = true;

BaseType_t task;
#ifndef CONFIG_FREERTOS_UNICORE
    task = xTaskCreatePinnedToCore(otaTask, "ota_task", 8192, params, 5, NULL, 1);
#else
    task = xTaskCreate(otaTask, "ota_task", 8192, params, 5, NULL);
#endif
  if (task != pdPASS) {
    ESP_LOGE(TAG, "Failed to create OTA task");
    delete params->state;
    delete params;
    httpd_resp_set_type(reqCopy, "application/json");
    httpd_resp_set_status(reqCopy, "500 Internal Server Error");
    httpd_resp_sendstr(reqCopy, "{\"success\":false,\"error\":\"Failed to create OTA task\"}");
    httpd_req_async_handler_complete(reqCopy);
    instance->m_otaInProgress = false;
    return ESP_FAIL; 
  }

  return ESP_OK;
}

void WebServerManager::otaTask(void *pvParameters) {
  OTAParams *params = static_cast<OTAParams *>(pvParameters);
  WebServerManager *instance = params->instance;
  httpd_req_t *req = params->req;
  
  params->state->currentUploadType = params->uploadType;
  params->state->skipReboot = params->skipReboot;
  params->state->totalBytes = params->contentLength;
  params->state->writtenBytes = 0;
  params->state->error.clear();

  params->state->handle = 0;
  params->state->updatePartition = nullptr;
  params->state->littlefsPartition = nullptr;

  ESP_LOGI(TAG, "Starting OTA task. Type: %d, Size: %zu", (int)params->uploadType, params->contentLength);

  const size_t buffer_size = 4096;
  char *buffer = (char *)heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!buffer) {
    params->state->error = "Buffer allocation failed";
    goto error;
  }

  if (params->uploadType == OTAUploadType::FIRMWARE) {
    params->state->updatePartition = esp_ota_get_next_update_partition(NULL);
    if (!params->state->updatePartition) {
       params->state->error = "No OTA partition";
       goto error;
    }
    if (esp_ota_begin(params->state->updatePartition, params->contentLength, &params->state->handle) != ESP_OK) {
       params->state->error = "OTA begin failed";
       goto error;
    }
  } else {
    params->state->littlefsPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!params->state->littlefsPartition) {
      params->state->error = "No LittleFS partition";
      goto error;
    }
    if (params->contentLength > params->state->littlefsPartition->size) {
      params->state->error = "Image too large";
      goto error;
    }
    LittleFS.end();
    if (esp_partition_erase_range(params->state->littlefsPartition, 0, params->state->littlefsPartition->size) != ESP_OK) {
      params->state->error = "Erase failed";
      goto error;
    }
  }

  {
    size_t remaining = params->contentLength;
    size_t last_broadcast = 0;
    int received;
    while (remaining > 0) {
        received = httpd_req_recv(req, buffer, std::min(remaining, buffer_size));
        if (received < 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            params->state->error = "Receive error";
            goto error;
        }
        
        if (received > 0) {
            if (params->uploadType == OTAUploadType::FIRMWARE) {
                if (esp_ota_write(params->state->handle, buffer, received) != ESP_OK) {
                    params->state->error = "Write error";
                    goto error;
                }
            } else {
                if (esp_partition_write(params->state->littlefsPartition, params->state->writtenBytes, buffer, received) != ESP_OK) {
                    params->state->error = "Write error";
                    goto error;
                }
            }
            params->state->writtenBytes += received;
            remaining -= received;
            
            if ((params->state->writtenBytes - last_broadcast) >= std::max(params->contentLength / 20, (size_t)1) || remaining == 0) {
                instance->broadcastOTAStatus(*params->state);
                last_broadcast = params->state->writtenBytes;
            }
        } else {
            params->state->error = "Received empty payload, aborting";
            goto error;
        }
    }
  }

  if (params->uploadType == OTAUploadType::FIRMWARE) {
    if (esp_ota_end(params->state->handle) != ESP_OK || esp_ota_set_boot_partition(params->state->updatePartition) != ESP_OK) {
        params->state->error = "End/SetBoot failed";
        goto error;
    }
  } else if (params->uploadType == OTAUploadType::LITTLEFS) {
    if(!LittleFS.begin()) {
      ESP_LOGE(TAG, "Failed to remount LittleFS after OTA");
      params->state->error = "LittleFS remount failed after OTA";
      goto error;
    }
  }

  {
    bool shouldReboot = !params->skipReboot;
    params->state->inProgress = false;
    instance->broadcastOTAStatus(*params->state);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Update Complete\"}");
    httpd_req_async_handler_complete(req);
    instance->m_otaInProgress = false;

    if (buffer) free(buffer);
    delete params->state;
    delete params;

    if (shouldReboot) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    vTaskDelete(NULL);
    return;
  }

error:
  params->state->inProgress = false;
  instance->broadcastOTAStatus(*params->state);
  if (params->state->handle) esp_ota_abort(params->state->handle);
  if (buffer) free(buffer);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_status(req, "500 Internal Server Error");
  std::string errJson = JsonBuilder::object()
      .addBool("success", false)
      .addString("error", params->state->error.c_str())
      .toStringUnformatted();
  httpd_resp_sendstr(req, errJson.c_str());
  httpd_req_async_handler_complete(req);
  instance->m_otaInProgress = false;
  delete params->state;
  delete params;
  vTaskDelete(NULL);
}

std::string WebServerManager::getOTAInfo() {
  JsonBuilder status = JsonBuilder::object();
  status.addString("type", "ota_info");
  
  status.addString("current_version",
                          esp_app_get_description()->version);

  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next_update = esp_ota_get_next_update_partition(NULL);
  if (running)
    status.addString("running_partition", running->label);
  if (next_update)
    status.addString("next_update_partition",
                            next_update->label);
  return status.toStringUnformatted();
}

void WebServerManager::broadcastOTAStatus(const OTAState& state) {
  JsonBuilder status = JsonBuilder::object();
  status.addString("type", "ota_status");
  if (!state.error.empty()) {
    status.addString("error", state.error.c_str());
  }
  status.addBool("in_progress", state.inProgress);
  status.addNumber("bytes_written", state.writtenBytes);
  status.addString("upload_type", (state.currentUploadType == OTAUploadType::LITTLEFS) ? "littlefs" : "firmware");

  if (state.inProgress && state.totalBytes > 0) {
    status.addNumber("progress_percent", (float)state.writtenBytes / state.totalBytes * 100.0f);
    status.addNumber("total_bytes", state.totalBytes);
  }
  
  std::string otaStatus = status.toStringUnformatted();
  broadcastWs((const uint8_t *)otaStatus.c_str(), otaStatus.size(), HTTPD_WS_TYPE_TEXT);
}

// ============================================================================
// Certificate Handlers
// ============================================================================

esp_err_t WebServerManager::handleCertificateUpload(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  char query[256], type_param[8];
  if(httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      (httpd_query_key_value(query, "type", type_param, sizeof(type_param)) != ESP_OK )) {
    return sendJsonError(req, "Missing 'type' parameter", "400 Bad Request");
  }
  
  char* end = nullptr;
  long type_int = strtol(type_param, &end, 10);
  if (end == type_param || *end != '\0' || type_int < 0 || type_int >= static_cast<long>(espConfig::CertType::MAX)) {
    return sendJsonError(req, "Invalid 'type' parameter", "400 Bad Request");
  }
  const espConfig::CertType type = static_cast<espConfig::CertType>(type_int);
  
  const size_t content_len = req->content_len;
  if (content_len == 0 || content_len > 8192) {
    return sendJsonError(req, "Invalid bundle content length", "400 Bad Request");
  }

  std::string certBuf;
  certBuf.reserve(content_len + 1);
  char buffer[1024];
  size_t remaining = content_len;

  while (remaining > 0) {
    size_t chunk_size = std::min(remaining, sizeof(buffer) - 1);
    int received = httpd_req_recv(req, buffer, chunk_size);
    if (received <= 0) {
      return sendJsonError(req, "Failed to receive bundle data", "400 Bad Request");
    }
    buffer[received] = '\0';
    certBuf.append(buffer, received);
    remaining -= received;
  }

  bool success = instance->m_configManager.saveCertificate(type, certBuf);

  if (success) {
    httpd_resp_set_type(req, "application/json");
    std::string response = JsonBuilder::object()
        .addBool("success", true)
        .addString("message", "Certificate saved successfully!")
        .addNumber("size", content_len)
        .toStringUnformatted();
    httpd_resp_send(req, response.c_str(), response.length());
    return ESP_OK;
  }

  return sendJsonError(req, "Failed to save certificate", HTTPD_500);
}

esp_err_t WebServerManager::handleCertificateStatus(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  JsonBuilder response = JsonBuilder::object();
  response.withObject("data", [&](JsonBuilder& certificates) {
    std::vector<CertificateStatus> status = instance->m_configManager.getCertificatesStatus();
    for (auto cert : status) {
      JsonBuilder certInfo = JsonBuilder::object();
      bool isPrivateKey = (cert.type == espConfig::CertType::MQTT_PRIVATE_KEY || cert.type == espConfig::CertType::HTTPS_PRIVATE_KEY);
      bool isCA = (cert.type == espConfig::CertType::MQTT_CA || cert.type == espConfig::CertType::HTTPS_CA_CERT);

      if (!isPrivateKey) {
        if (!cert.issuer.empty()) certInfo.addString("issuer", cert.issuer.c_str());
        if (!cert.subject.empty()) certInfo.addString("subject", cert.subject.c_str());
        if (!cert.serial.empty()) certInfo.addString("serial", cert.serial.c_str());
        if (!cert.fingerprint.empty()) certInfo.addString("fingerprint", cert.fingerprint.c_str());
        if (!cert.expiration.from.empty() && !cert.expiration.to.empty()) {
          certInfo.withObject("expiration", [&](JsonBuilder& exp) {
            exp.addString("from", cert.expiration.from.c_str());
            exp.addString("to", cert.expiration.to.c_str());
          });
        }
        if(!isCA){
          certInfo.addBool("keyMatchesCert", cert.keyMatchesCert);
        }
      } else {
        certInfo.addBool("exists", true);
        certInfo.addString("keyType", cert.keyType.c_str());
      }
      certificates.addItem(std::to_string(static_cast<uint8_t>(cert.type)).c_str(), std::move(certInfo).release());
    }
  });
  response.addBool("success", true);

  std::string resp = response.toStringUnformatted();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp.c_str(), resp.length());
  return ESP_OK;
}


esp_err_t WebServerManager::handleCertificateDelete(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if(!instance->basicAuth(req)){
    return sendAuthFailure(req);
  }
  if (!instance) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  char query[256], type_param[8];
  if(httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      (httpd_query_key_value(query, "type", type_param, sizeof(type_param)) != ESP_OK )) {
    return sendJsonError(req, "Missing 'type' parameter", "400 Bad Request");
  }
  
  char* end = nullptr;
  long type_int = strtol(type_param, &end, 10);
  if (end == type_param || *end != '\0' || type_int < 0 || type_int >= static_cast<long>(espConfig::CertType::MAX)) {
    return sendJsonError(req, "Invalid 'type' parameter", "400 Bad Request");
  }
  const espConfig::CertType type = static_cast<espConfig::CertType>(type_int);

  bool success = instance->m_configManager.deleteCertificate(type);

  if (success) {
    httpd_resp_set_type(req, "application/json");
    std::string response = JsonBuilder::object()
        .addBool("success", true)
        .addString("message", "Certificate deleted")
        .toStringUnformatted();
    httpd_resp_send(req, response.c_str(), response.length());
    return ESP_OK;
  }

  return sendJsonError(req, "Failed to delete certificate", HTTPD_500);
}

// ============================================================================
// Household / node / backup / recovery / provisioning endpoints
// ============================================================================

namespace {

std::string hexEncodeBytes(const std::vector<uint8_t> &bytes) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

int hexDigitVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> hexDecodeBytes(const std::string &in) {
    std::vector<uint8_t> out;
    if (in.size() % 2 != 0) return out;
    for (size_t i = 0; i < in.size(); i += 2) {
        const int hi = hexDigitVal(in[i]);
        const int lo = hexDigitVal(in[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

void sendJsonStr(httpd_req_t *req, const std::string &body, const char *status = nullptr) {
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body.c_str());
}

/**
 * @brief Whether this request is a browser navigating rather than an API client calling.
 *
 * Six API paths - household, node, health, security, audit and backup - are also Web UI
 * page names. Handlers are matched before the catch-all, so without this check loading,
 * reloading or bookmarking those pages returned raw JSON instead of the app: the UI only
 * worked when reached by clicking a link inside the already-loaded app, because that never
 * asks the server.
 *
 * The distinguisher is the Accept header. Browsers send "text/html,..."; API clients send
 * "application/json", or the wildcard that curl and python-requests send, which must stay
 * JSON so that scripted access to these endpoints keeps working.
 */
bool wantsHtml(httpd_req_t *req) {
  size_t len = httpd_req_get_hdr_value_len(req, "Accept");
  if (len == 0 || len >= 128) {
    return false;
  }
  char buf[128];
  if (httpd_req_get_hdr_value_str(req, "Accept", buf, sizeof(buf)) != ESP_OK) {
    return false;
  }
  return std::string(buf).find("text/html") != std::string::npos;
}

} // namespace

esp_err_t WebServerManager::handleGetHousehold(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    // See wantsHtml(): `/household` is both this endpoint and a Web UI page.
    if (wantsHtml(req)) return handleRootOrHash(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_householdManager) {
        return sendJsonError(req, "Household manager unavailable", "503 Service Unavailable");
    }
    const auto &hh = instance->m_householdManager->info();
    // The salt is reported because it is not a secret - it travels in the backup header and
    // is a KDF input rather than a key - and because nothing else on the device exposes it.
    // The household command key is BLAKE2b(recovery_secret || salt), so a client that can
    // only obtain the secret derives a key that can never match, and authenticated lock
    // control ends up impossible to configure rather than merely unconfigured.
    sendJsonStr(req, fmt::format(
        "{{\"household_id\":\"{}\",\"household_name\":\"{}\",\"state\":\"{}\",\"config_version\":{},"
        "\"trust_key\":\"{}\",\"recovery_metadata\":\"{}\",\"recovery_salt\":\"{}\","
        "\"has_recovery_secret\":{},\"recovery_exported\":{}}}",
        jsonEscape(hh.household_id), jsonEscape(hh.household_name),
        household::householdStateToString(hh.state), hh.config_version,
        hexEncodeBytes(hh.trust_public_key), hexEncodeBytes(hh.recovery_metadata),
        hexEncodeBytes(instance->m_householdManager->recoverySalt()),
        hh.has_recovery_secret ? "true" : "false",
        hh.recovery_exported ? "true" : "false"));
    return ESP_OK;
}

esp_err_t WebServerManager::handleGetNode(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    // See wantsHtml(): `/node` is both this endpoint and a Web UI page.
    if (wantsHtml(req)) return handleRootOrHash(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_nodeIdentityManager) {
        return sendJsonError(req, "Node manager unavailable", "503 Service Unavailable");
    }
    const auto &nd = instance->m_nodeIdentityManager->info();
    sendJsonStr(req, fmt::format(
        "{{\"node_id\":\"{}\",\"node_name\":\"{}\",\"node_role\":\"{}\",\"node_state\":\"{}\","
        "\"household_id\":\"{}\",\"generation\":{},\"public_key\":\"{}\","
        "\"cert_fingerprint\":\"{}\"}}",
        jsonEscape(nd.node_id), jsonEscape(nd.node_name),
        household::nodeRoleToString(nd.node_role), household::nodeStateToString(nd.state),
        jsonEscape(nd.household_id), nd.generation, hexEncodeBytes(nd.public_key),
        hexEncodeBytes(nd.cert_fingerprint)));
    return ESP_OK;
}

esp_err_t WebServerManager::handleGetHealth(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    // See wantsHtml(): `/health` is both this endpoint and a Web UI page.
    if (wantsHtml(req)) return handleRootOrHash(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_healthManager) {
        return sendJsonError(req, "Health manager unavailable", "503 Service Unavailable");
    }
    sendJsonStr(req, instance->m_healthManager->toJson(instance->m_healthManager->snapshot()));
    return ESP_OK;
}

esp_err_t WebServerManager::handleGetSecurity(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    // See wantsHtml(): `/security` is both this endpoint and a Web UI page.
    if (wantsHtml(req)) return handleRootOrHash(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_securityManager) {
        return sendJsonError(req, "Security manager unavailable", "503 Service Unavailable");
    }
    sendJsonStr(req, instance->m_securityManager->toJson(instance->m_securityManager->compute()));
    return ESP_OK;
}

esp_err_t WebServerManager::handleGetAudit(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    // See wantsHtml(): `/audit` is both this endpoint and a Web UI page.
    if (wantsHtml(req)) return handleRootOrHash(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_auditManager) {
        return sendJsonError(req, "Audit manager unavailable", "503 Service Unavailable");
    }
    sendJsonStr(req, instance->m_auditManager->toJson(200));
    return ESP_OK;
}

esp_err_t WebServerManager::handleGetBackup(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    // See wantsHtml(): `/backup` is both this endpoint and a Web UI page.
    if (wantsHtml(req)) return handleRootOrHash(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_backupManager) {
        return sendJsonError(req, "Backup manager unavailable", "503 Service Unavailable");
    }
    sendJsonStr(req, fmt::format("{{\"last_backup_time\":{},\"last_backup_hash\":\"{}\"}}",
                                 instance->m_backupManager->lastBackupTime(),
                                 hexEncodeBytes(instance->m_backupManager->lastBackupHash())));
    return ESP_OK;
}

esp_err_t WebServerManager::handleSetIssuerName(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance->basicAuth(req)) return sendAuthFailure(req);

  // A bounded body: this endpoint carries an id and a short name and nothing else.
  std::string body;
  if (!readBody(req, body, 512)) {
    return ESP_OK; // readBody has already answered
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    return sendJsonError(req, "Invalid JSON", "400 Bad Request");
  }
  const cJSON *idNode = cJSON_GetObjectItemCaseSensitive(root, "issuerId");
  const cJSON *nameNode = cJSON_GetObjectItemCaseSensitive(root, "name");
  const bool shaped = cJSON_IsString(idNode) && idNode->valuestring != nullptr &&
                      (nameNode == nullptr || cJSON_IsString(nameNode));
  const std::string issuerIdHex = shaped ? idNode->valuestring : "";
  // An absent or empty name clears the label. Clearing has to be possible, and an empty
  // box in a form must mean "clear" rather than silently doing nothing - a no-op that
  // looks like a save is worse than an explicit removal.
  const std::string label =
      shaped && nameNode != nullptr && nameNode->valuestring != nullptr
          ? nameNode->valuestring
          : "";
  cJSON_Delete(root);

  if (!shaped) {
    return sendJsonError(req, "Expected {\"issuerId\":\"<hex>\",\"name\":\"...\"}",
                         "400 Bad Request");
  }

  const std::vector<uint8_t> issuerId = hexDecodeBytes(issuerIdHex);
  const auto readerData = instance->m_readerDataManager.snapshot();
  const bool known =
      std::any_of(readerData.issuers.begin(), readerData.issuers.end(),
                  [&issuerId](const ddk::Issuer &issuer) {
                    return issuer.id.size() == issuerId.size() &&
                           std::equal(issuer.id.begin(), issuer.id.end(),
                                      issuerId.begin());
                  });
  if (!known) {
    // Refused rather than stored: a label for a pairing that does not exist would never
    // be shown, and a mistyped id would look like it had worked.
    return sendJsonError(req, "Unknown issuer", "404 Not Found");
  }

  if (!instance->m_readerDataManager.setIssuerLabel(issuerId, label)) {
    return sendJsonError(req, "Name is not acceptable (max 64 characters, printable)",
                         "400 Bad Request");
  }
  instance->m_readerDataManager.save();
  ESP_LOGI(TAG, "Issuer label %s", label.empty() ? "cleared" : "set");
  sendJsonStr(req, "{\"success\":true}");
  return ESP_OK;
}

// ============================================================================
// Home Assistant direct API (/api/ha/*)
//
// A second way into the same data as the Web UI, for a client that has neither a
// broker nor a browser. See HA_INTEGRATION_PLAN.md.
// ============================================================================

namespace {

/// Bumped only when a client would misread a response, so the component can refuse a
/// device it does not understand rather than guess at it.
constexpr int kHaProtocolVersion = 1;

/// Translate the health manager's internal backup wording to the documented one.
///
/// ``HealthManager`` records a successful backup as ``"ok"`` while the household
/// contract (and the value published on ``B/backup/status``) is ``"completed"``. Both
/// describe the same event, so the API boundary normalises instead of emitting a value
/// the contract does not define - a client validating strictly against the contract
/// would otherwise reject it.
std::string documentedBackupStatus(const std::string &healthStatus) {
  return healthStatus == "ok" ? "completed" : healthStatus;
}

/// Find the most recent HomeKey authentication in the audit log.
///
/// Read from the audit log rather than a dedicated field because that is where the
/// firmware already records exactly this event. Scanning for the highest sequence
/// number avoids assuming an iteration order for the ring buffer.
bool lastHomeKeyAuth(const AuditManager &audit, uint32_t &timestampOut, bool &successOut) {
  bool found = false;
  uint32_t bestSeq = 0;
  for (const auto &record : audit.records()) {
    const bool isAuth = record.event_type == AuditManager::HOMEKEY_AUTH_SUCCESS ||
                        record.event_type == AuditManager::HOMEKEY_AUTH_FAILURE;
    if (!isAuth) continue;
    if (found && record.seq <= bestSeq) continue;
    found = true;
    bestSeq = record.seq;
    timestampOut = record.timestamp;
    successOut = record.event_type == AuditManager::HOMEKEY_AUTH_SUCCESS;
  }
  return found;
}

/// Name a ``LockManager`` state for a client that should not have to hard-code the
/// firmware's numeric enum. The spelling matches the household contract's lock-state
/// vocabulary, so a client can use one set of names for every transport.
const char *lockStateToName(int state) {
  switch (state) {
    case LockManager::UNLOCKED: return "unlocked";
    case LockManager::LOCKED: return "locked";
    case LockManager::JAMMED: return "jammed";
    case LockManager::UNLOCKING: return "unlocking";
    case LockManager::LOCKING: return "locking";
    default: return "unknown";
  }
}

} // namespace

bool WebServerManager::haRequireTls(httpd_req_t *req) const {
  if (m_tlsActive.load(std::memory_order_relaxed)) {
    return true;
  }
  sendJsonError(req,
                "HTTPS is not active on this device, so this data would be sent in the "
                "clear. Enable HTTPS under Misc -> Security and retry over https.",
                "503 Service Unavailable");
  return false;
}

void WebServerManager::startHttpsRedirectServer() {
  if (m_redirectServer != nullptr) {
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  // One catch-all is all this needs, and only two sockets: a redirect is cheap to serve
  // and the client leaves immediately.
  config.max_uri_handlers = 2;
  config.max_open_sockets = 2;
  config.stack_size = 4096;
  config.lru_purge_enable = true;
  config.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&m_redirectServer, &config) != ESP_OK) {
    ESP_LOGW(TAG, "Could not listen on port 80; reach the Web UI with https:// directly");
    m_redirectServer = nullptr;
    return;
  }

  httpd_uri_t redirect = {.uri = "/*",
                          .method = HTTP_GET,
                          .handler = handleHttpRedirect,
                          .user_ctx = nullptr};
  if (httpd_register_uri_handler(m_redirectServer, &redirect) != ESP_OK) {
    ESP_LOGW(TAG, "Could not register the HTTP redirect handler");
    httpd_stop(m_redirectServer);
    m_redirectServer = nullptr;
    return;
  }
  ESP_LOGI(TAG, "Port 80 redirects to https:// so existing addresses keep working");
}

esp_err_t WebServerManager::handleHttpRedirect(httpd_req_t *req) {
  // Only reachable while TLS is active, so the target scheme is always https.
  std::string targetHost;

  char host[128] = {0};
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
      host[0] != '\0') {
    std::string candidate(host);
    const size_t colon = candidate.find(':');
    if (colon != std::string::npos) {
      candidate.erase(colon);
    }
    // Accept only a bare hostname or IPv4 literal. Anything else - a slash, an @, a
    // space - could turn this into an open redirect, so it is rejected and the device's
    // own address is used instead.
    const size_t bad = candidate.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-");
    if (!candidate.empty() && bad == std::string::npos) {
      targetHost = candidate;
    }
  }

  if (targetHost.empty()) {
    // The Arduino String must outlive the c_str() call, hence the named local.
    const String localAddress = WiFi.localIP().toString();
    targetHost = localAddress.c_str();
  }

  const std::string location = fmt::format("https://{}{}", targetHost, req->uri);
  httpd_resp_set_status(req, "301 Moved Permanently");
  httpd_resp_set_hdr(req, "Location", location.c_str());
  // A redirect has no body worth sending.
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t WebServerManager::handleHaInfo(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);

  // Not authenticated, and deliberately so: this is the request a client makes to learn
  // the fingerprint it is about to ask its user to confirm, and it cannot ask for a
  // password before knowing what it is talking to. Nothing here is new information -
  // every field below is already broadcast in the mDNS TXT record, which is equally
  // public on the LAN.
  const auto &misc = instance->m_configManager.getConfig<espConfig::misc_config_t>();
  const auto &certs = instance->m_configManager.getHttpsCertsConfig();
  const std::string fingerprint = deviceCert::certificateFingerprint(certs.serverCert);
  const bool tls = instance->isTlsActive();

  JsonBuilder info = JsonBuilder::object();
  info.addNumber("protocol", kHaProtocolVersion);
  info.addString("transport", tls ? "tls" : "plaintext");
  info.addBool("secure", tls);
  info.addNumber("port", instance->getServerPort());
  info.addString("fingerprint", fingerprint);
  info.addBool("setup_completed", misc.setupCompleted);
  info.withObject("device", [&](JsonBuilder &d) {
    d.addString("name", misc.deviceName);
    d.addString("model", "HomeKey-ESP32");
    d.addString("firmware", esp_app_get_description()->version);
    // Arduino's String, not const char*, so it needs converting for the builder.
    const String macAddress = WiFi.macAddress();
    d.addString("mac", macAddress.c_str());
    if (instance->m_nodeIdentityManager) {
      d.addString("node_id", instance->m_nodeIdentityManager->info().node_id);
      d.addString("node_name", instance->m_nodeIdentityManager->info().node_name);
    }
  });
  // The household id is deliberately not exposed here even when one exists: it is not in
  // the mDNS TXT record, and it forms part of the MQTT topic path, so publishing it
  // unauthenticated would hand out more than discovery already does.
  info.withObject("capabilities", [](JsonBuilder &c) {
    c.addBool("read_state", true);
    c.addBool("write_config", true);
    c.addBool("lock_control", true);
  });

  sendJsonStr(req, info.toStringUnformatted());
  return ESP_OK;
}

esp_err_t WebServerManager::handleHaState(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance->basicAuth(req)) return sendAuthFailure(req);
  if (!instance->haRequireTls(req)) return ESP_OK;

  JsonBuilder state = JsonBuilder::object();
  state.addNumber("protocol", kHaProtocolVersion);
  state.addString("firmware", esp_app_get_description()->version);

  // Identity. Present here and deliberately absent from /api/ha/info: the household id
  // forms part of the MQTT topic path, and /api/ha/info is unauthenticated so that a
  // client can read the fingerprint before it has anything to authenticate with. A client
  // that reaches this endpoint has already authenticated.
  if (instance->m_householdManager) {
    const auto &hh = instance->m_householdManager->info();
    state.addString("household_id", hh.household_id);
    state.addString("household_name", hh.household_name);
    state.addString("household_state", household::householdStateToString(hh.state));
    state.addNumber("config_version", hh.config_version);
  }
  if (instance->m_nodeIdentityManager) {
    const auto &nd = instance->m_nodeIdentityManager->info();
    state.addString("node_id", nd.node_id);
    state.addString("node_name", nd.node_name);
    state.addString("node_role", household::nodeRoleToString(nd.node_role));
    state.addString("node_state", household::nodeStateToString(nd.state));
    state.addNumber("generation", nd.generation);
  }

  state.withObject("wifi", [](JsonBuilder &w) {
    w.addBool("connected", WiFi.status() == WL_CONNECTED);
    w.addNumber("rssi", WiFi.RSSI());
  });

  if (instance->m_healthManager) {
    const HealthManager::Snapshot snapshot = instance->m_healthManager->snapshot();
    // The health payload is embedded verbatim - the same JSON the household MQTT transport
    // publishes on ``B/health``. Sending the identical document is the point: a client
    // cannot interpret one transport's health differently from the other's, because there
    // is only one implementation of it.
    if (auto health = parse_json(instance->m_healthManager->toJson(snapshot))) {
      state.addItem("health", std::move(*health));
    } else {
      ESP_LOGW(TAG, "Could not embed the health payload in /api/ha/state");
    }
    // The compact security state, exactly as ``B/security`` publishes it: OK or WARNING,
    // never a numeric score.
    state.addString("security", snapshot.security_all_ok ? "OK" : "WARNING");
    state.addString("backup_status", documentedBackupStatus(snapshot.backup_status));
  }

  // The origin of the most recent lock change, so a client can report *who* opened the
  // door rather than only that it opened. Reported alongside the health document
  // because it explains the lock state that document carries. No timestamp: this
  // response is a snapshot, so every field in it is current by construction, and the
  // push transport is the one that has to say when.
  if (instance->m_lockManager != nullptr) {
    state.withObject("lock_last", [&](JsonBuilder &l) {
      l.addNumber("current", instance->m_lockManager->getCurrentState());
      l.addNumber("target", instance->m_lockManager->getTargetState());
      l.addString("source",
                  LockManager::sourceName(static_cast<uint8_t>(
                      instance->m_lockManager->lastChangeSource())));
    });
  }

  // Omitted entirely when no HomeKey authentication has ever been recorded, so a client
  // sees "never" rather than a fabricated success at time zero.
  if (instance->m_auditManager) {
    uint32_t authTimestamp = 0;
    bool authSuccess = false;
    if (lastHomeKeyAuth(*instance->m_auditManager, authTimestamp, authSuccess)) {
      state.withObject("last_auth", [&](JsonBuilder &a) {
        // "HomeKey" matches the type the MQTT transport publishes on ``B/last_auth``.
        a.addString("type", "HomeKey");
        a.addString("result", authSuccess ? "SUCCESS" : "FAILURE");
        a.addNumber("timestamp", static_cast<double>(authTimestamp));
      });
    }
  }

  sendJsonStr(req, state.toStringUnformatted());
  return ESP_OK;
}

esp_err_t WebServerManager::handleHaConfig(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  if (!instance->basicAuth(req)) return sendAuthFailure(req);
  if (!instance->haRequireTls(req)) return ESP_OK;

  // Reuse the Web UI's own handlers instead of a parallel implementation, so secret
  // masking, validation and the MASKED_SECRET write guard behave identically on both
  // surfaces and cannot drift apart. Both read the same `?type=` parameter.
  return req->method == HTTP_GET ? handleGetConfig(req) : handleSaveConfig(req);
}

esp_err_t WebServerManager::handleHaLock(httpd_req_t *req) {
  WebServerManager *instance = getInstance(req);
  // Authorisation is the same device credential the rest of the Web UI uses, and it is
  // the strongest thing this device has: the direct API is reached over TLS with the
  // node's certificate pinned, so the credential is never exposed to the local network
  // in the clear. Commands are POST-only so a page the user merely visits cannot drive
  // the lock.
  if (!instance->basicAuth(req)) return sendAuthFailure(req);
  if (!instance->haRequireTls(req)) return ESP_OK;

  if (instance->m_lockManager == nullptr) {
    return sendJsonError(req, "No lock is configured on this device",
                         "409 Conflict");
  }

  // A tiny fixed body: this endpoint deliberately cannot carry anything else.
  char body[128] = {0};
  const int received = httpd_req_recv(req, body, sizeof(body) - 1);
  if (received <= 0) {
    return sendJsonError(req, "Missing request body", "400 Bad Request");
  }
  body[received] = '\0';

  cJSON *root = cJSON_Parse(body);
  if (root == nullptr) {
    return sendJsonError(req, "Invalid JSON", "400 Bad Request");
  }
  const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
  const bool usable = cJSON_IsString(action) && action->valuestring != nullptr;
  const std::string requested = usable ? action->valuestring : "";
  cJSON_Delete(root);

  if (requested != "lock" && requested != "unlock") {
    return sendJsonError(req,
                         "Expected {\"action\":\"lock\"} or {\"action\":\"unlock\"}",
                         "400 Bad Request");
  }

  const uint8_t target =
      requested == "lock" ? LockManager::LOCKED : LockManager::UNLOCKED;
  instance->m_lockManager->setTargetState(target, LockManager::WEB);
  ESP_LOGI(TAG, "Home Assistant requested %s", requested.c_str());

  // Report the state the command produced, taken from the lock manager after the
  // command rather than from the request: a jammed or failed mechanism must not read
  // back as success.
  JsonBuilder result = JsonBuilder::object();
  result.addString("action", requested);
  result.addString("state", lockStateToName(instance->m_lockManager->getCurrentState()));
  result.addNumber("current", instance->m_lockManager->getCurrentState());
  result.addNumber("target", instance->m_lockManager->getTargetState());
  sendJsonStr(req, result.toStringUnformatted());
  return ESP_OK;
}

esp_err_t WebServerManager::handleCreateBackup(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_backupManager) {
        return sendJsonError(req, "Backup manager unavailable", "503 Service Unavailable");
    }
    const std::vector<uint8_t> blob = instance->m_backupManager->createBackup();
    if (blob.empty()) {
        return sendJsonError(req, "Backup creation failed (no household/node identity?)", "500 Internal Server Error");
    }
    sendJsonStr(req, fmt::format("{{\"success\":true,\"backup\":\"{}\"}}", hexEncodeBytes(blob)));
    return ESP_OK;
}

esp_err_t WebServerManager::handleRestoreBackup(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_restoreManager) {
        return sendJsonError(req, "Restore manager unavailable", "503 Service Unavailable");
    }

    // A backup of the whole configuration is legitimately large, so the cap is generous -
    // but it is a heap allocation, not a stack one. This was a 16 KiB local array on the
    // same 6 KiB stack, so restoring a backup panicked the device every single time.
    std::string body;
    if (!readBody(req, body, 32 * 1024)) {
        return ESP_OK; // readBody has already answered
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) {
        return sendJsonError(req, "Invalid JSON", "400 Bad Request");
    }
    const cJSON *secretNode = cJSON_GetObjectItemCaseSensitive(root, "secret");
    const cJSON *backupNode = cJSON_GetObjectItemCaseSensitive(root, "backup");
    const bool ok = cJSON_IsString(secretNode) && cJSON_IsString(backupNode);
    if (!ok) {
        cJSON_Delete(root);
        return sendJsonError(req, "Missing 'secret' or 'backup'", "400 Bad Request");
    }
    const std::vector<uint8_t> secret = hexDecodeBytes(secretNode->valuestring);
    const std::vector<uint8_t> blob = hexDecodeBytes(backupNode->valuestring);
    cJSON_Delete(root);

    std::string error;
    if (!instance->m_restoreManager->restore(blob, secret, error)) {
        return sendJsonError(req, error, "400 Bad Request");
    }
    sendJsonStr(req, "{\"success\":true,\"message\":\"Restore completed\"}");
    return ESP_OK;
}

esp_err_t WebServerManager::handleExportRecovery(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_householdManager) {
        return sendJsonError(req, "Household manager unavailable", "503 Service Unavailable");
    }
    std::vector<uint8_t> secret;
    if (!instance->m_householdManager->exportRecoverySecretOnce(secret)) {
        return sendJsonError(req, "Recovery secret already exported or unavailable", "409 Conflict");
    }
    // The salt goes with the secret, because the two are only useful together: the command
    // key is BLAKE2b(secret || salt). This is the one moment the user has the secret in
    // hand, so it is the moment to hand over everything needed to actually use it.
    sendJsonStr(req, fmt::format(
        "{{\"success\":true,\"recovery_secret\":\"{}\",\"recovery_salt\":\"{}\"}}",
        hexEncodeBytes(secret),
        hexEncodeBytes(instance->m_householdManager->recoverySalt())));
    return ESP_OK;
}

esp_err_t WebServerManager::handleIssueProvisioning(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_provisioningManager) {
        return sendJsonError(req, "Provisioning manager unavailable", "503 Service Unavailable");
    }
    const uint32_t ttlSeconds = 600;
    const std::string code = instance->m_provisioningManager->issueCode(ttlSeconds);
    if (code.empty()) {
        return sendJsonError(req, "Could not issue provisioning code", "500 Internal Server Error");
    }
    // The code is a secret; it is returned once and never logged. The TTL is
    // included so the UI can display the expiry.
    sendJsonStr(req, fmt::format("{{\"success\":true,\"code\":\"{}\",\"ttl_seconds\":{}}}",
                                 code, ttlSeconds));
    return ESP_OK;
}

esp_err_t WebServerManager::handleJoinHousehold(httpd_req_t *req) {
    WebServerManager *instance = getInstance(req);
    if (!instance->basicAuth(req)) return sendAuthFailure(req);
    if (!instance->m_provisioningManager || !instance->m_householdManager ||
        !instance->m_nodeIdentityManager) {
        return sendJsonError(req, "Managers unavailable", "503 Service Unavailable");
    }

    // Small body - a code, an id, a name and a role - and read from the heap rather than the
    // stack. This was a 4096-byte local array on a 6144-byte stack, so every join overflowed
    // it: the device panicked, rebooted, and came back with the household half written at
    // PROVISIONING and no response ever sent to the caller.
    std::string body;
    if (!readBody(req, body, 1024)) {
        return ESP_OK; // readBody has already answered
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) {
        return sendJsonError(req, "Invalid JSON", "400 Bad Request");
    }
    const cJSON *codeNode = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *idNode = cJSON_GetObjectItemCaseSensitive(root, "household_id");
    const cJSON *nameNode = cJSON_GetObjectItemCaseSensitive(root, "household_name");
    const bool ok = cJSON_IsString(codeNode) && cJSON_IsString(idNode);
    if (!ok) {
        cJSON_Delete(root);
        return sendJsonError(req, "Missing 'code' or 'household_id'", "400 Bad Request");
    }
    const std::string code = codeNode->valuestring;
    const std::string householdId = idNode->valuestring;
    const std::string householdName = cJSON_IsString(nameNode) ? nameNode->valuestring : "Household";

    std::vector<uint8_t> trustKey;
    const cJSON *trustNode = cJSON_GetObjectItemCaseSensitive(root, "trust_key");
    if (cJSON_IsString(trustNode)) {
        trustKey = hexDecodeBytes(trustNode->valuestring);
    }
    const household::NodeRole role =
        household::nodeRoleFromString(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "node_role"))
                                          ? cJSON_GetObjectItemCaseSensitive(root, "node_role")->valuestring
                                          : "other");
    const std::string nodeName =
        cJSON_IsString(cJSON_GetObjectItemCaseSensitive(root, "node_name"))
            ? cJSON_GetObjectItemCaseSensitive(root, "node_name")->valuestring
            : householdName;
    cJSON_Delete(root);

    if (!instance->m_provisioningManager->validateAndConsume(code)) {
        return sendJsonError(req, "Invalid, expired or already-used provisioning code", "401 Unauthorized");
    }

    // Every one of these five writes is the same write as far as the user is concerned:
    // the household record. A handler that reports success on a write which did not land
    // describes a device that looks enrolled until it reboots and unconfigured after -
    // which is what a household stuck at PROVISIONING is. So each result is checked, and
    // a failure is reported as a failure rather than as an enrollment.
    const bool joined = instance->m_householdManager->joinHousehold(householdId, householdName, trustKey);
    const bool roleStored = instance->m_nodeIdentityManager->setRole(role);
    const bool householdStored = instance->m_nodeIdentityManager->setHousehold(householdId);
    const bool nodeActive = instance->m_nodeIdentityManager->setState(household::NodeState::ACTIVE);
    const bool completed = instance->m_householdManager->completeProvisioning();

    if (!joined || !roleStored || !householdStored || !nodeActive || !completed) {
        // The usual cause is a full NVS partition. Report the numbers here rather than
        // making the user attach a serial console to find out why nothing stuck: the
        // NVS partition is small, and the audit ring takes most of it.
        nvs_stats_t stats{};
        nvs_get_stats(nullptr, &stats);
        ESP_LOGE(TAG,
                 "Enrollment in household %s was NOT stored (household=%d role=%d "
                 "node=%d complete=%d; NVS entries used %lu of %lu). After a reboot this "
                 "device is not a member.",
                 householdId.c_str(), joined, roleStored, householdStored, completed,
                 static_cast<unsigned long>(stats.used_entries),
                 static_cast<unsigned long>(stats.total_entries));
        return sendJsonError(req,
                             "The household record could not be stored because the device's "
                             "non-volatile storage is full, so nothing was enrolled. Reboot "
                             "and check the log for the NVS entry counts.",
                             "507 Insufficient Storage");
    }
    if (instance->m_auditManager) {
        instance->m_auditManager->record(AuditManager::NODE_ENROLLMENT, AuditManager::SOURCE_WEB,
                                         AuditManager::RESULT_SUCCESS,
                                         instance->m_nodeIdentityManager->info().node_id, householdId);
    }
    sendJsonStr(req, "{\"success\":true,\"message\":\"Node enrolled in household\"}");
    return ESP_OK;
}
