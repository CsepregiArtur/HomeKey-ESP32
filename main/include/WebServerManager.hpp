#pragma once
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include <cstdint>
#include <deque>
#include <memory>
#include <atomic>
#include <string>
#include <vector>

// Forward declarations
class ConfigManager;
class NvsCredentialStore;
class SystemManager;
class MqttManager;
class NfcManager;
class HouseholdManager;
class NodeIdentityManager;
class SecurityManager;
class HealthManager;
class AuditManager;
class ProvisioningManager;
class BackupManager;
class RestoreManager;
namespace loggable {
class WebSocketLogSinker;
}

// ============================================================================
// WebSocket Frame Structures
// ============================================================================

struct WsFrame {
  int fd;
  httpd_ws_type_t type;
  size_t len;
  uint8_t *payload;
  static constexpr size_t INLINE_SIZE = 128;
  uint8_t inlinePayload[INLINE_SIZE];
};

struct WsFrameDeleter {
  void operator()(WsFrame *frame) const {
    if (frame && frame->payload != frame->inlinePayload)
      delete[] frame->payload;
    delete frame;
  }
};

using WsFramePtr = std::unique_ptr<WsFrame, WsFrameDeleter>;

// ============================================================================
// WebServerManager Class
// ============================================================================

class WebServerManager {
public:
  // ------------------------------------------------------------------------
  // Public Interface
  // ------------------------------------------------------------------------
  WebServerManager(ConfigManager &configManager,
                   NvsCredentialStore &readerDataManager);
  ~WebServerManager();

  void begin();

  /**
   * @brief Stops the web server and cleans up all resources.
   *
   * Performs a complete shutdown of the web server by stopping the HTTP server,
   * deleting the WebSocket task and queue, and stopping/deleting the status timer.
   */
  void end();
  bool basicAuth(httpd_req_t* req);
  void setMqttManager(MqttManager *mqttManager) { m_mqttManager = mqttManager; }
  void setNfcManager(NfcManager *nfcManager) { m_nfcManager = nfcManager; }
  void setHouseholdManager(HouseholdManager *h) { m_householdManager = h; }
  void setNodeIdentityManager(NodeIdentityManager *n) { m_nodeIdentityManager = n; }
  void setSecurityManager(SecurityManager *s) { m_securityManager = s; }
  void setHealthManager(HealthManager *h) { m_healthManager = h; }
  void setAuditManager(AuditManager *a) { m_auditManager = a; }
  void setProvisioningManager(ProvisioningManager *p) { m_provisioningManager = p; }
  void setBackupManager(BackupManager *b) { m_backupManager = b; }
  void setRestoreManager(RestoreManager *r) { m_restoreManager = r; }
  void broadcastWs(const uint8_t *payload, size_t len, httpd_ws_type_t type);
  void setWSBackLogSize(const uint16_t size);

  /**
   * @brief Number of WebSocket frames dropped because the send queue was full.
   *
   * Frames are dropped without blocking when the queue is full; this counter
   * makes that backpressure observable (e.g. in the metrics broadcast).
   */
  [[nodiscard]] uint64_t getWsFrameDropCount() const {
    return m_wsFrameDropped.load(std::memory_order_relaxed);
  }

private:
  // ------------------------------------------------------------------------
  // Internal Types & Enums
  // ------------------------------------------------------------------------

  struct WsClient {
    int fd;
    std::mutex mutex;
    WsClient(int file_descriptor) : fd(file_descriptor) {}
  };

  enum class OTAUploadType { FIRMWARE, LITTLEFS };

  struct OTAState {
    esp_ota_handle_t handle = 0;
    const esp_partition_t *updatePartition = nullptr;
    const esp_partition_t *littlefsPartition = nullptr;
    size_t writtenBytes = 0;
    size_t totalBytes = 0;
    bool skipReboot = false;
    bool inProgress = false;
    std::string error;
    OTAUploadType currentUploadType = OTAUploadType::FIRMWARE;
  };

  struct OTAParams {
    httpd_req_t *req;
    WebServerManager *instance;
    OTAUploadType uploadType;
    bool skipReboot;
    size_t contentLength;
    OTAState *state;
  };

  /**
   * @brief Work item for the "update from GitHub" path.
   *
   * Unlike OTAParams this carries no httpd_req_t: the image comes from GitHub over
   * esp_http_client rather than from a browser upload, so the task owns the whole
   * download -> flash -> reboot sequence.
   */
  struct GithubOtaParams {
    WebServerManager *instance;
    bool developmentChannel;
    OTAState *state;
  };

  // ------------------------------------------------------------------------
  // Static Task Callbacks
  // ------------------------------------------------------------------------
  static void ws_send_task(void *arg);
  static void otaTask(void *pvParameters);
  static void githubOtaTask(void *pvParameters);
  static void statusTimerCallback(void *arg);

  // ------------------------------------------------------------------------
  // HTTP Route Handlers (Static)
  // ------------------------------------------------------------------------
  static esp_err_t handleGetConfig(httpd_req_t *req);
  static esp_err_t handleGetEthConfig(httpd_req_t *req);
  static esp_err_t handleGetNfcPresets(httpd_req_t *req);
  static esp_err_t handleClearConfig(httpd_req_t *req);
  static esp_err_t handleSaveConfig(httpd_req_t *req);
  static esp_err_t handleReboot(httpd_req_t *req);
  static esp_err_t handleHKReset(httpd_req_t *req);
  static esp_err_t handleWifiReset(httpd_req_t *req);
  static esp_err_t handleStartConfigAP(httpd_req_t *req);
  static esp_err_t handleRootOrHash(httpd_req_t *req);
  static esp_err_t handleStaticFiles(httpd_req_t *req);
  static esp_err_t handleWebSocket(httpd_req_t *req);
  static esp_err_t handleOTAUpload(httpd_req_t *req);
  static esp_err_t handleGetReleaseInfo(httpd_req_t *req);
  static esp_err_t handleInstallRelease(httpd_req_t *req);
  static esp_err_t handleCertificateUpload(httpd_req_t *req);
  static esp_err_t handleCertificateStatus(httpd_req_t *req);
  static esp_err_t handleCertificateDelete(httpd_req_t *req);

  // Household / node / backup / recovery / provisioning endpoints
  static esp_err_t handleGetHousehold(httpd_req_t *req);
  static esp_err_t handleGetNode(httpd_req_t *req);
  static esp_err_t handleGetHealth(httpd_req_t *req);
  static esp_err_t handleGetSecurity(httpd_req_t *req);
  static esp_err_t handleGetAudit(httpd_req_t *req);
  static esp_err_t handleGetBackup(httpd_req_t *req);
  static esp_err_t handleCreateBackup(httpd_req_t *req);
  static esp_err_t handleRestoreBackup(httpd_req_t *req);
  static esp_err_t handleExportRecovery(httpd_req_t *req);
  static esp_err_t handleIssueProvisioning(httpd_req_t *req);
  static esp_err_t handleJoinHousehold(httpd_req_t *req);

  static void captivePortalSaveTask(void* pvParameters);
  static void captivePortalEthSaveTask(void* pvParameters);
  static esp_err_t handleCaptivePortal(httpd_req_t *req);
  static esp_err_t handleGetCaptivePortalConfig(httpd_req_t *req);
  static esp_err_t handleSaveCaptivePortalConfig(httpd_req_t *req);
  static esp_err_t handleWifiScan(httpd_req_t *req);

  // ------------------------------------------------------------------------
  // Core Internal Methods
  // ------------------------------------------------------------------------

  // Server setup
  void setupRoutes();
  void setupCaptivePortalRoutes();

  // WebSocket management
  void addWebSocketClient(int fd);
  void removeWebSocketClient(int fd);
  void queue_ws_frame(int fd, const uint8_t *payload, size_t len,
                      httpd_ws_type_t type);
  esp_err_t handleWebSocketMessage(httpd_req_t *req,
                                   const std::string &message);

  // Device info/status
  std::string getDeviceMetrics();
  std::string getDeviceInfo();
  std::string getOTAInfo();
  // OTA management
  void broadcastOTAStatus(const OTAState& state);

  // Utility methods
  static bool validateRequest(httpd_req_t *req, cJSON *currentData,
                              cJSON *obj);
  /// Reject requests whose Host header does not name this device (DNS rebinding).
  static bool hostHeaderAllowed(httpd_req_t *req);
  static WebServerManager *getInstance(httpd_req_t *req);
  static esp_err_t sendAuthFailure(httpd_req_t *req);
  static esp_err_t ws_post_handshake_cb(httpd_req_t *req);
  static esp_err_t sendJsonError(httpd_req_t *req, const std::string &msg, 
                            const char *status = "400 Bad Request");
  static bool heapGuardOk(httpd_req_t *req, bool otherActive,
                                    const char *thisName, const char *otherName);
  bool shouldEnableHttps() const;

  // ------------------------------------------------------------------------
  // Member Variables
  // ------------------------------------------------------------------------

  // HTTP Server
  httpd_handle_t m_server;
  static const char *TAG;
  std::string m_sessionId;
  HouseholdManager *m_householdManager = nullptr;
  NodeIdentityManager *m_nodeIdentityManager = nullptr;
  SecurityManager *m_securityManager = nullptr;
  HealthManager *m_healthManager = nullptr;
  AuditManager *m_auditManager = nullptr;
  ProvisioningManager *m_provisioningManager = nullptr;
  BackupManager *m_backupManager = nullptr;
  RestoreManager *m_restoreManager = nullptr;

  // True while the captive-portal route set is installed, i.e. while the device is
  // in setup AP mode. That portal has to stay reachable without Web UI credentials,
  // otherwise a user who lost the password cannot get back into their own device.
  bool m_captivePortalMode = false;
  // Consecutive failed Web UI logins since boot, used to slow down guessing.
  uint32_t m_authFailureCount = 0;

  // Dependencies
  ConfigManager &m_configManager;
  NvsCredentialStore &m_readerDataManager;
  MqttManager *m_mqttManager;
  NfcManager *m_nfcManager;

  // WebSocket infrastructure
  QueueHandle_t m_wsQueue;
  TaskHandle_t m_wsTaskHandle;
  std::vector<std::unique_ptr<WsClient>> m_wsClients;
  std::mutex m_wsClientsMutex;
  esp_timer_handle_t m_statusTimer;
  std::deque<std::vector<uint8_t>> m_wsBroadcastBuffer;
  std::atomic<uint16_t> wsBacklogSize{0};
  std::atomic<uint64_t> m_wsFrameDropped{0};
  std::atomic<bool> m_otaInProgress{false};
  bool m_isInitialized{false};
};
