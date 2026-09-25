#include <cstdint>
#include <memory>
#include "ConsoleLogSinker.h"
#include "HomeSpan.h"
#include "config.hpp"
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif_sntp.h>
#include <ctime>
#include "dns_server.h"
#include "HomeKitLock.hpp"
#include "LockManager.hpp"
#include "NfcManager.hpp"
#include "ConfigManager.hpp"
#include "DeviceCert.hpp"
#include "DiscoveryAdvertiser.hpp"
#include "ReaderDataManager.hpp"
#include "HardwareManager.hpp"
#include "MqttManager.hpp"
#include "WebServerManager.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sodium/crypto_sign.h>
#include <sodium/crypto_box.h>
#include <sodium/randombytes.h>
#include "HAP.h"
#include "loggable.hpp"
#include "loggable_espidf.hpp"
#include "WebSocketLogSinker.h"
#include "lwip/inet.h"
#include "nvs_flash.h"
#include "household_types.hpp"
#include "HouseholdManager.hpp"
#include "NodeIdentityManager.hpp"
#include "ProvisioningManager.hpp"
#include "SecurityManager.hpp"
#include "AuditManager.hpp"
#include "HealthManager.hpp"
#include "BackupManager.hpp"
#include "RestoreManager.hpp"
#include "eventStructs.hpp"
#include "app_event_loop.hpp"
#include <span>
#include <system_error>

std::unique_ptr<LockManager> lockManager;
NvsCredentialStore readerDataManager;
ConfigManager configManager;
std::unique_ptr<HardwareManager> hardwareManager;
std::unique_ptr<MqttManager> mqttManager;
WebServerManager webServerManager(configManager, readerDataManager);
DiscoveryAdvertiser discoveryAdvertiser;
std::unique_ptr<HomeKitLock> homekitLock;
std::unique_ptr<NfcManager> nfcManager;

// Household / multi-node managers.
std::unique_ptr<HouseholdManager> householdManager;
std::unique_ptr<NodeIdentityManager> nodeIdentityManager;
std::unique_ptr<ProvisioningManager> provisioningManager;
std::unique_ptr<SecurityManager> securityManager;
std::unique_ptr<AuditManager> auditManager;
std::unique_ptr<HealthManager> healthManager;
std::unique_ptr<BackupManager> backupManager;
std::unique_ptr<RestoreManager> restoreManager;

// Held-open subscriptions for the audit hooks and backup status relay.
static AppEventLoop::SubscriptionHandle s_auditNfcSub;
static AppEventLoop::SubscriptionHandle s_auditLockSub;
static AppEventLoop::SubscriptionHandle s_backupDoneSub;
static AppEventLoop::SubscriptionHandle s_backupFailSub;

static dns_server_handle_t dns_server = NULL;

bool pollHS = false;

// ============================================================================
// First-run security setup
// ============================================================================

/**
 * @brief Handle first-run credential setup and report any shipped defaults still in use.
 *
 * Every default this firmware ships with (HomeKit Setup Code, setup AP password, OTA
 * password and Web UI credentials) is published in the source repository, so a device
 * that keeps them can be paired with, reconfigured or reflashed by anybody who can
 * reach it over the network.
 *
 * Nothing is generated here. A freshly flashed device comes up with the shipped
 * placeholders, Web UI authentication off and `setupCompleted` false, and the Web UI
 * shows a blocking onboarding screen where the user chooses every secret deliberately:
 * Web UI password, HomeKit setup code, OTA password and setup AP password. Submitting
 * that screen sets `setupCompleted`, after which this function only reports anything
 * still left on a shipped default.
 *
 * An earlier version generated random secrets instead and printed them once to the
 * serial log. That was removed: the print happens during the hard reset performed by
 * `idf.py flash`, before any monitor is attached, so it is easily missed - and once
 * missed there was no way to recover the setup AP password without dumping the flash.
 *
 * Must run before the Web UI, MQTT and HomeSpan are started.
 */
static void securityInit() {
  static const char *TAG = "Security";
  const espConfig::misc_config_t &misc = configManager.getConfig<espConfig::misc_config_t>();

  // Migration for devices configured before `setupCompleted` existed. ConfigManager falls
  // back to the struct default (false) when the key is absent from NVS, so without this an
  // already-configured device would be pushed through onboarding again after an upgrade.
  // A stored, non-placeholder Web UI password proves the device was set up already.
  if (!misc.setupCompleted && configManager.hasStoredConfig() &&
      !misc.webPassword.empty() && misc.webPassword != WEB_AUTH_PASSWORD) {
    const std::string payload = "{\"setupCompleted\":true}";
    if (!configManager.updateFromJson<espConfig::misc_config_t>(payload).empty() &&
        configManager.saveConfig<espConfig::misc_config_t>()) {
      ESP_LOGI(TAG, "Existing Web UI credentials found; treating first-run setup as complete.");
    } else {
      ESP_LOGE(TAG, "Could not record the completed first-run setup state.");
    }
    return;
  }

  if (!misc.setupCompleted) {
    ESP_LOGW(TAG, "=============== FIRST-RUN SETUP REQUIRED ===============");
    ESP_LOGW(TAG, "This device is using the shipped default credentials.");
    ESP_LOGW(TAG, "Open the web UI and complete the setup screen to set your own.");
    ESP_LOGW(TAG, "  Setup AP password  : %s", AP_PASSWORD);
    ESP_LOGW(TAG, "  HomeKit Setup Code : %s (change it before pairing)", SETUP_CODE);
    ESP_LOGW(TAG, "The web UI has no password until setup is completed, so keep this");
    ESP_LOGW(TAG, "device on a trusted network and never expose it to the internet.");
    ESP_LOGW(TAG, "=======================================================");
    return;
  }

  // Fully configured device: report anything that is still on a shipped default
  // instead of changing it behind the user's back.
  if (misc.otaPasswd.empty() || misc.otaPasswd == OTA_PWD) {
    ESP_LOGW(TAG, "HomeSpan OTA is disabled while the OTA password is the shipped default. "
                  "Set your own under Misc -> HomeSpan to enable it.");
  }
  if (misc.accessPointPassword == AP_PASSWORD) {
    ESP_LOGW(TAG, "The setup access point still uses the password published in the source "
                  "repository. Change it in the setup portal or under Misc -> Security.");
  }
  if (misc.setupCode == SETUP_CODE) {
    ESP_LOGW(TAG, "The HomeKit Setup Code is still the shipped default and is public. "
                  "Change it while no controller is paired.");
  }
  if (!misc.webAuthEnabled) {
    ESP_LOGW(TAG, "Web UI authentication is disabled: anyone who can reach this device on the "
                  "network can read its configuration, reset the pairing and flash firmware. "
                  "Enable a username/password under Misc -> Security.");
  }
#if CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT
  ESP_LOGI(TAG, "OTA images are signature verified (signed apps, no secure boot).");
#else
  ESP_LOGI(TAG, "OTA images are not signature verified; see docs/content/security.md to enable it.");
#endif
}

static void dhcp_set_captiveportal_url(void) {
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip_info);

    char ip_addr[16];
    inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
    ESP_LOGI("Main", "Setting up captive portal on IP: %s", ip_addr);

    char captiveportal_uri[32];
    snprintf(captiveportal_uri, sizeof(captiveportal_uri), "http://%s", ip_addr);

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(netif));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, captiveportal_uri, strlen(captiveportal_uri)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(netif));
}

static void start_captive_portal(void)
{
    dhcp_set_captiveportal_url();

    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns_server = start_dns_server(&dns_config);
    ESP_LOGI("Main", "DNS server started for captive portal");
}

namespace {

/**
 * Start SNTP once the station interface is up.
 *
 * The device has no RTC and nothing else ever sets the clock: wallClockSeconds() in
 * MqttManager and AuditManager deliberately falls back to seconds since boot while time()
 * looks unset. With no time source that fallback is permanent, so every timestamp the
 * firmware reports - lock changes, HomeKey authorisations, audit records, backups - is an
 * uptime rather than a date, and "when did this door open" has no answer to give.
 *
 * Started here because SNTP needs a route to somewhere, and asynchronously because nothing
 * should wait for the first reply.
 */
void startNetworkTime() {
  static bool started = false;
  if (started) {
    return;
  }
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  const esp_err_t err = esp_netif_sntp_init(&config);
  if (err != ESP_OK) {
    ESP_LOGW("Time", "Could not start SNTP (%s); timestamps stay as seconds since boot.",
             esp_err_to_name(err));
    return;
  }
  started = true;
  ESP_LOGI("Time", "SNTP started against pool.ntp.org.");
}

/// Report the first successful sync once, so a boot log says whether times became real.
void reportClockSync() {
  static bool reported = false;
  // The same threshold wallClockSeconds() uses to decide the clock is real, rather than a
  // separate status call: one definition of "time is set" is easier to keep true.
  if (reported || time(nullptr) <= 1000000000) {
    return;
  }
  reported = true;
  ESP_LOGI("Time", "Clock synced: epoch %lld.", static_cast<long long>(time(nullptr)));
}

} // namespace

std::function<void(int)> lambda = [](int status) {
  if (status == 1) {
    char identifier[18];
    sprintf(identifier, "%.2s%.2s%.2s%.2s%.2s%.2s", HAPClient::accessory.ID, HAPClient::accessory.ID + 3, HAPClient::accessory.ID + 6, HAPClient::accessory.ID + 9, HAPClient::accessory.ID + 12, HAPClient::accessory.ID + 15);
    mqttManager->begin(std::string(identifier));
    webServerManager.begin(); 
    // The web server only starts once the station interface is up, so this is the first
    // moment the API has a real port worth advertising.
    discoveryAdvertiser.onNetworkUp();
    // The same moment and the same reason: there is a route for SNTP to reach a server on.
    startNetworkTime();
  } else if (status == 0){
    pollHS = false;
    mqttManager->end();
    webServerManager.end();
    // No station interface and no TLS in AP mode, so the API is not advertised there.
    discoveryAdvertiser.stop();
    WiFi.mode(WIFI_AP_STA);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    const std::string macStr = fmt::format("HK_{:02X}{:02X}{:02X}{:02X}", mac[2], mac[3], mac[4], mac[5]);
    auto misc = configManager.getConfig<espConfig::misc_config_t>();
    // WPA2-PSK + CCMP is used deliberately instead of WPA2/WPA3 mixed mode with
    // AES-CMAC. The setup AP exists to let any phone or laptop join and configure
    // the device, and mixed-mode WPA3 cipher suites cause association failures
    // ("connection timeout") on a range of older clients. CCMP is universally
    // supported; WPA3-only hardening belongs on the station side, not on a
    // short-lived provisioning AP.
    WiFi.softAP(macStr.c_str(), misc.accessPointPassword.c_str(), 11, false, 2, false, WIFI_AUTH_WPA2_PSK, WIFI_CIPHER_TYPE_CCMP); 
    start_captive_portal();
    webServerManager.begin();
    // The setup AP exists so that a device without Wi-Fi credentials can still be
    // configured, but it has no reason to stay on the air forever. Cycling it while
    // nobody is connected shortens the window in which it can be attacked, and it
    // never interrupts an actual configuration session because a connected client
    // resets the counter.
#if AP_IDLE_CYCLE_MIN > 0
    uint32_t idleSeconds = 0;
#endif
    while(true){
#if AP_IDLE_CYCLE_MIN > 0
      vTaskDelay(pdMS_TO_TICKS(1000));
      if(WiFi.softAPgetStationNum() > 0){
        idleSeconds = 0;
      } else if(++idleSeconds >= (uint32_t)(AP_IDLE_CYCLE_MIN * 60)){
        idleSeconds = 0;
        ESP_LOGW("Main", "Setup AP idle for %d min with no client - restarting it.", AP_IDLE_CYCLE_MIN);
        WiFi.softAPdisconnect(true);
        vTaskDelay(pdMS_TO_TICKS(1000));
        WiFi.softAP(macStr.c_str(), configManager.getConfig<espConfig::misc_config_t>().accessPointPassword.c_str(), 11, false, 2, false, WIFI_AUTH_WPA2_PSK, WIFI_CIPHER_TYPE_CCMP);
        dhcp_set_captiveportal_url();
      }
#else
      vTaskDelay(pdMS_TO_TICKS(100));
#endif
    }
  }
};
using namespace loggable;

bool initLogging(){
  uint8_t logLevel;
  if(!configManager.getNVSLogLevel(logLevel)) return false;
  uint16_t backlogMaxSize = 0;
  if(!configManager.getBacklogMaxSize(backlogMaxSize)) return false;
  webServerManager.setWSBackLogSize(backlogMaxSize);
  esp_log_level_set("*", static_cast<esp_log_level_t>(logLevel));
  loggable::Sinker::instance().set_level(static_cast<loggable::LogLevel>(logLevel));
  loggable::espidf::LogHook::install(false, true);
  SinkConfig console_cfg{};
  console_cfg.delivery = SinkConfig::Delivery::Sync;
  console_cfg.name = "console";
  Sinker::instance().add_sinker(std::make_shared<loggable::ConsoleLogSinker>(), console_cfg);
  SinkConfig ws_cfg{};
  ws_cfg.delivery = SinkConfig::Delivery::Async;
  ws_cfg.queue_capacity = 256;
  ws_cfg.max_batch = 16;
  ws_cfg.name = "log_ws";
  Sinker::instance().add_sinker(std::make_shared<loggable::WebSocketLogSinker>(webServerManager), ws_cfg);
  return true;
}

/**
 * @brief Subscribe the audit log to the security-relevant events that occur on this node.
 *
 * HomeKey tap results and lock/unlock transitions are the primary local events;
 * web login and MQTT command audits are recorded by their respective managers.
 */
static void setupAuditHooks() {
  const std::string nodeId = nodeIdentityManager ? nodeIdentityManager->info().node_id : "";
  s_auditNfcSub = AppEventLoop::subscribe(NFC_EVENT, NFC_TAP_EVENT, [nodeId](const uint8_t *data, size_t size) {
    if (!data || size == 0) return;
    std::span<const uint8_t> payload(data, size);
    std::error_code ec;
    NfcEvent nfc_event = alpaca::deserialize<NfcEvent>(payload, ec);
    if (ec) return;
    if (nfc_event.type == HOMEKEY_TAP) {
      EventHKTap s = alpaca::deserialize<EventHKTap>(nfc_event.data, ec);
      if (ec) return;
      auditManager->record(s.status ? AuditManager::HOMEKEY_AUTH_SUCCESS
                                    : AuditManager::HOMEKEY_AUTH_FAILURE,
                           AuditManager::SOURCE_NFC,
                           s.status ? AuditManager::RESULT_SUCCESS : AuditManager::RESULT_FAILURE,
                           nodeId);
    }
  });
  s_auditLockSub = AppEventLoop::subscribe(LOCK_EVENT, LOCK_STATE_CHANGED, [nodeId](const uint8_t *data, size_t size) {
    if (!data || size == 0) return;
    std::span<const uint8_t> payload(data, size);
    std::error_code ec;
    EventLockState s = alpaca::deserialize<EventLockState>(payload, ec);
    if (ec) return;
    if (s.currentState == 0) {
      auditManager->record(AuditManager::UNLOCK, AuditManager::SOURCE_LOCAL,
                           AuditManager::RESULT_SUCCESS, nodeId);
    } else if (s.currentState == 1) {
      auditManager->record(AuditManager::LOCK, AuditManager::SOURCE_LOCAL,
                           AuditManager::RESULT_SUCCESS, nodeId);
    }
  });
}

/**
 * @brief Initialize runtime, configure logging/serial, and instantiate core subsystem managers.
 *
 * Initializes the global runtime infrastructure (Sinker), sets logging levels and Serial,
 * constructs and assigns global manager instances (ReaderDataManager, ConfigManager, WebServerManager,
 * HardwareManager, LockManager, MqttManager, HomeKitLock, NfcManager), reads NFC-related configuration,
 * and starts managers that require explicit startup.
 *
 * @note This function allocates and assigns globals used across the application and invokes their
 *       initialization routines (calls to `begin()` where applicable). It also logs the resolved NFC
 *       GPIO pin configuration based on persisted settings.
 */
void setup() {
  #ifdef CONFIG_IDF_TARGET_ESP32
  gpio_set_pull_mode(GPIO_NUM_3, GPIO_PULLUP_ONLY); // U0RXD idle-HIGH in case UART-bridge not present
  #endif
  #ifdef CONFIG_INIT_ARDU_SERIAL_LOGGING
  Serial.begin(115200);
  #endif
  if(esp_err_t err = nvs_flash_init(); err != ESP_OK){
    ESP_LOGE("Main", "Failed to initialize NVS. Aborting. err=%d", err);
    return;
  }
  if(!initLogging()){
    ESP_LOGE("Main", "Could not initialize logging. Aborting.");
    return;
  }
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK) {
    ESP_LOGE("Main", "Failed to create default event loop: %d", err);
  }
  // Why did we just boot? Without this a crash-reboot is indistinguishable in
  // the logs from a hang: the log simply stops and later resumes. The reset
  // reason separates a software panic from a watchdog timeout from a brownout,
  // which need entirely different fixes.
  {
    const esp_reset_reason_t why = esp_reset_reason();
    const char *name = "unknown";
    switch (why) {
      case ESP_RST_POWERON:  name = "power-on"; break;
      case ESP_RST_EXT:      name = "external pin"; break;
      case ESP_RST_SW:       name = "software restart"; break;
      case ESP_RST_PANIC:    name = "PANIC (exception / assert)"; break;
      case ESP_RST_INT_WDT:  name = "INTERRUPT WATCHDOG"; break;
      case ESP_RST_TASK_WDT: name = "TASK WATCHDOG"; break;
      case ESP_RST_WDT:      name = "other watchdog"; break;
      case ESP_RST_DEEPSLEEP:name = "deep sleep wake"; break;
      case ESP_RST_BROWNOUT: name = "BROWNOUT (supply dipped)"; break;
      case ESP_RST_SDIO:     name = "SDIO"; break;
      default: break;
    }
    const bool unexpected = (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT ||
                             why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ||
                             why == ESP_RST_BROWNOUT);
    if (unexpected) {
      ESP_LOGE("Boot", "*** UNEXPECTED RESET: %s (reason %d) ***", name, (int)why);
    } else {
      ESP_LOGI("Boot", "Reset reason: %s (%d)", name, (int)why);
    }
  }

  configManager.begin();
  // Must happen before any manager starts so the Web UI, MQTT and HomeSpan all
  // come up using the credentials this generates on a factory-fresh device.
  securityInit();

  // Give the device its own TLS identity before anything can serve HTTPS. Done here,
  // rather than lazily on the first HTTPS request, so the fingerprint is available to
  // advertise over mDNS and to log once at boot.
  bool certificateGenerated = false;
  deviceCert::ensureSelfSignedCertificate(configManager, &certificateGenerated);

  // One-time HTTPS enablement. Until now a device could hold a certificate and still serve
  // plain HTTP, which would leave the Home Assistant API nothing safe to talk to. Offer it
  // once, record that the offer was made, then leave the setting alone for good - so a user
  // who deliberately turns HTTPS back off is not fought on every boot.
  const auto &miscCfg = configManager.getConfig<espConfig::misc_config_t>();
  if (!miscCfg.httpsAutoEnabledOnce) {
    const auto &certs = configManager.getHttpsCertsConfig();
    const bool hasCertificate = !certs.serverCert.empty() && !certs.privateKey.empty();
    const bool enableHttps = hasCertificate && !miscCfg.webHttpsEnabled;
    const std::string payload = enableHttps
        ? "{\"httpsAutoEnabledOnce\":true,\"webHttpsEnabled\":true}"
        : "{\"httpsAutoEnabledOnce\":true}";
    if (configManager.updateFromJson<espConfig::misc_config_t>(payload).empty() ||
        !configManager.saveConfig<espConfig::misc_config_t>()) {
      ESP_LOGE("Security", "Could not record the one-time HTTPS migration state.");
    } else if (enableHttps) {
      ESP_LOGW("Security", "Enabled HTTPS using the %s certificate. Browsers will warn "
                           "until it is accepted; compare the fingerprint logged above.",
               certificateGenerated ? "newly generated" : "stored");
    }
  }

  // --- Household / node model (never erases existing config or HomeKey data) ---
  householdManager = std::make_unique<HouseholdManager>();
  householdManager->begin();
  householdManager->migrate();
  nodeIdentityManager = std::make_unique<NodeIdentityManager>();
  nodeIdentityManager->begin();
  provisioningManager = std::make_unique<ProvisioningManager>();
  provisioningManager->begin();
  auditManager = std::make_unique<AuditManager>();
  auditManager->begin();
  securityManager = std::make_unique<SecurityManager>(configManager);
  healthManager = std::make_unique<HealthManager>();
  healthManager->begin();

  hardwareManager = std::make_unique<HardwareManager>(configManager.getConfig<espConfig::actions_config_t>());
  lockManager = std::make_unique<LockManager>(configManager.getConfig<espConfig::misc_config_t>(), configManager.getConfig<espConfig::actions_config_t>());
  mqttManager = std::make_unique<MqttManager>(configManager);
  homekitLock = std::make_unique<HomeKitLock>(lambda, *lockManager, configManager, readerDataManager);
  espConfig::misc_config_t miscConfig = configManager.getConfig<espConfig::misc_config_t>();
  // A node identity is minted once per device (migration) so every node has a
  // stable id from here on. Replacing a node later always creates a NEW identity.
  if (!nodeIdentityManager->hasIdentity()) {
    nodeIdentityManager->generateIdentity(household::NodeRole::OTHER, miscConfig.deviceName);
  }
  static const char* TAG = "Main";
  if(miscConfig.nfcPinsPreset != PIN_UNSET){
    ESP_LOGI(TAG, "NFC GPIO pins preset: %s", nfcGpioPinsPresets[miscConfig.nfcPinsPreset].name.c_str());
    ESP_LOGI(TAG, "NFC preset pins: %d, %d, %d, %d", nfcGpioPinsPresets[miscConfig.nfcPinsPreset].gpioPins[0], nfcGpioPinsPresets[miscConfig.nfcPinsPreset].gpioPins[1], nfcGpioPinsPresets[miscConfig.nfcPinsPreset].gpioPins[2], nfcGpioPinsPresets[miscConfig.nfcPinsPreset].gpioPins[3]);
  } else {
    ESP_LOGI(TAG, "NFC GPIO pins preset: Custom");
    ESP_LOGI(TAG, "NFC Custom GPIO pins: %d, %d, %d, %d", miscConfig.nfcGpioPins[0], miscConfig.nfcGpioPins[1], miscConfig.nfcGpioPins[2], miscConfig.nfcGpioPins[3]);
  }
  // Resolve the pins once. NfcManager is handed the preset array when one is
  // selected, so logging miscConfig.nfcGpioPins here would report SDA/SCL that
  // the reader is not actually using.
  const std::array<uint8_t, 4> &activeNfcPins =
      miscConfig.nfcPinsPreset == PIN_UNSET ? miscConfig.nfcGpioPins
                                            : nfcGpioPinsPresets[miscConfig.nfcPinsPreset].gpioPins;
  const char *readerName = miscConfig.nfcReaderType == 0   ? "PN532 (SPI)"
                           : miscConfig.nfcReaderType == 1 ? "PN7160"
                           : miscConfig.nfcReaderType == 2 ? "ST25R3916 (I2C)"
                                                           : "UNKNOWN";
  ESP_LOGI(TAG, "NFC reader type: %s (%u)", readerName, miscConfig.nfcReaderType);
  if (miscConfig.nfcReaderType == 1) {
    ESP_LOGI(TAG, "NFC IRQ pin: %d, VEN pin: %d", miscConfig.nfcIrqPin, miscConfig.nfcVenPin);
  } else if (miscConfig.nfcReaderType == 2) {
    ESP_LOGI(TAG, "NFC I2C pins: SDA=%d, SCL=%d", activeNfcPins[0], activeNfcPins[1]);
  }
  readerDataManager.begin();

  nfcManager = std::make_unique<NfcManager>(readerDataManager,
                              activeNfcPins,
                              miscConfig.nfcReaderType,
                              miscConfig.nfcIrqPin,
                              miscConfig.nfcVenPin,
                              miscConfig.hkAuthPrecomputeEnabled,
                              miscConfig.nfcFastPollingEnabled);
  nfcManager->begin();

  webServerManager.setNfcManager(nfcManager.get());
  webServerManager.setMqttManager(mqttManager.get());
  webServerManager.setHouseholdManager(householdManager.get());
  webServerManager.setNodeIdentityManager(nodeIdentityManager.get());
  webServerManager.setSecurityManager(securityManager.get());
  webServerManager.setHealthManager(healthManager.get());
  // Needed by the Home Assistant API (/api/ha/state) to report lock state.
  webServerManager.setLockManager(lockManager.get());
  webServerManager.setAuditManager(auditManager.get());
  webServerManager.setProvisioningManager(provisioningManager.get());
  mqttManager->setHouseholdManager(householdManager.get());
  mqttManager->setNodeIdentityManager(nodeIdentityManager.get());
  mqttManager->setHealthManager(healthManager.get());
  mqttManager->setAuditManager(auditManager.get());
  // Lets the household last_auth topic report the name a user gave a paired controller.
  mqttManager->setReaderDataManager(&readerDataManager);

  healthManager->setSecurityManager(securityManager.get());
  healthManager->setNfcManager(nfcManager.get());
  healthManager->setMqttManager(mqttManager.get());
  healthManager->setLockManager(lockManager.get());
  backupManager = std::make_unique<BackupManager>(*householdManager, *nodeIdentityManager,
                                                  configManager, readerDataManager, *auditManager);
  backupManager->begin();
  restoreManager = std::make_unique<RestoreManager>(*householdManager, *nodeIdentityManager,
                                                    configManager, readerDataManager, *auditManager);
  restoreManager->begin();
  webServerManager.setBackupManager(backupManager.get());
  webServerManager.setRestoreManager(restoreManager.get());
  setupAuditHooks();

  s_backupDoneSub = AppEventLoop::subscribe(BACKUP_EVENT, BACKUP_COMPLETED,
    [](const uint8_t *, size_t) { if (mqttManager) mqttManager->publishBackupStatus("completed"); });
  s_backupFailSub = AppEventLoop::subscribe(BACKUP_EVENT, BACKUP_FAILED,
    [](const uint8_t *, size_t) { if (mqttManager) mqttManager->publishBackupStatus("failed"); });

  hardwareManager->begin();
  homekitLock->begin();
  lockManager->begin();
  // Records what to advertise. The service is not published here because HomeSpan's
  // status callback starts the web server and is what gives the API a port to advertise.
  discoveryAdvertiser.begin(configManager, *nodeIdentityManager, webServerManager);
  pollHS = true;
}
/**
 * @brief Run the main application loop: service HomeSpan events and yield to the RTOS.
*
 * Polls HomeSpan to process HomeKit and internal events, then delays 50 ms to allow other
 * FreeRTOS tasks to run.
 */

void loop() {
  if(pollHS)
    homeSpan.poll();

  // Self-heals the mDNS record if HomeSpan tore the responder down underneath us.
  discoveryAdvertiser.refresh();

  // Logs once when SNTP first answers, so the boot log says whether times are real yet.
  reportClockSync();

  // Publish household node telemetry on a slow cadence so the MQTT entities
  // (state/health/security/backup) stay fresh without hammering NVS or MQTT.
  static int64_t lastNodePublishUs = 0;
  const int64_t nowUs = esp_timer_get_time();
  if (nowUs - lastNodePublishUs >= 30 * 1000000LL) {
    lastNodePublishUs = nowUs;
    if (mqttManager && mqttManager->isConnected()) {
      mqttManager->publishNodeStatus();
    }
  }

  vTaskDelay(pdMS_TO_TICKS(50));
}
