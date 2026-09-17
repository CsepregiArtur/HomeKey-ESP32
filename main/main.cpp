#include <cstdint>
#include <memory>
#include "ConsoleLogSinker.h"
#include "HomeSpan.h"
#include "config.hpp"
#include <esp_event.h>
#include "dns_server.h"
#include "HomeKitLock.hpp"
#include "LockManager.hpp"
#include "NfcManager.hpp"
#include "ConfigManager.hpp"
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

std::unique_ptr<LockManager> lockManager;
NvsCredentialStore readerDataManager;
ConfigManager configManager;
std::unique_ptr<HardwareManager> hardwareManager;
std::unique_ptr<MqttManager> mqttManager;
WebServerManager webServerManager(configManager, readerDataManager);
std::unique_ptr<HomeKitLock> homekitLock;
std::unique_ptr<NfcManager> nfcManager;

static dns_server_handle_t dns_server = NULL;

bool pollHS = false;

// ============================================================================
// First-boot security defaults
// ============================================================================

namespace {

// Ambiguous glyphs (0/O, 1/l/I) are left out: these passwords are meant to be
// copied off a serial console or a setup page by hand.
constexpr char kPasswordAlphabet[] =
    "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";

constexpr size_t kPasswordAlphabetSize = sizeof(kPasswordAlphabet) - 1;

/// Setup Code generated on first boot, to be handed to HomeSpan once it is running.
std::string pendingSetupCode;

std::string randomPassword(size_t length) {
  std::string out(length, '\0');
  std::array<uint8_t, 32> buffer{};
  size_t written = 0;
  while (written < length) {
    randombytes_buf(buffer.data(), buffer.size());
    for (uint8_t byte : buffer) {
      // Rejection sampling keeps the distribution uniform although the alphabet
      // size does not divide 256. It also makes modulo bias impossible.
      if (byte >= 256 - (256 % kPasswordAlphabetSize)) {
        continue;
      }
      out[written++] = kPasswordAlphabet[byte % kPasswordAlphabetSize];
      if (written == length) {
        break;
      }
    }
  }
  return out;
}

bool isWeakSetupCode(const std::string &code) {
  static constexpr std::array<const char *, 12> kWeakCodes = {
      "00000000", "11111111", "22222222", "33333333", "44444444", "55555555",
      "66666666", "77777777", "88888888", "99999999", "12345678", "87654321"};
  return std::find(kWeakCodes.begin(), kWeakCodes.end(), code) != kWeakCodes.end();
}

/// Random HAP-valid Setup Code: 8 digits, no leading zero, none of the trivial patterns.
std::string randomSetupCode() {
  std::string code;
  std::array<uint8_t, 8> digits{};
  do {
    randombytes_buf(digits.data(), digits.size());
    code.clear();
    code += static_cast<char>('1' + (digits[0] % 9)); // HAP rejects a leading zero
    for (size_t i = 1; i < digits.size(); ++i) {
      code += static_cast<char>('0' + (digits[i] % 10));
    }
  } while (isWeakSetupCode(code));
  return code;
}

} // namespace

/**
 * @brief Replace the shipped credentials with per-device ones on a factory-fresh device.
 *
 * Every default this firmware ships with (HomeKit Setup Code, setup AP password,
 * OTA password, Web UI credentials and access point password) is published in the
 * source repository, so a device that keeps them can be paired with, reconfigured
 * or reflashed by anybody who can reach it over the network.
 *
 * Enabling device-wide protections such as flash encryption would require erasing
 * the flash and re-provisioning every already-deployed device, which is not an
 * option here. Generating the secrets only on first boot, on the other hand, needs
 * no migration: as soon as a configuration blob exists in NVS the stored values win
 * and this function never touches the credentials again - it only reports how the
 * device is configured so the remaining factory defaults are visible in the log.
 *
 * Must run before the Web UI, MQTT and HomeSpan are started so that everything
 * comes up using the freshly generated values. The Setup Code is the one exception:
 * HomeSpan owns the SRP verification data, so the generated code is queued in
 * pendingSetupCode and applied once HomeSpan is running.
 */
static void securityInit() {
  static const char *TAG = "Security";
  const espConfig::misc_config_t &misc = configManager.getConfig<espConfig::misc_config_t>();

  if (!configManager.hasStoredConfig()) {
    const std::string setupCode = randomSetupCode();
    const std::string apPassword = randomPassword(16);
    const std::string otaPassword = randomPassword(20);
    const std::string webPassword = randomPassword(16);
    const std::string payload = fmt::format(
        "{{\"setupCode\":\"{}\",\"accessPointPassword\":\"{}\",\"otaPasswd\":\"{}\","
        "\"webAuthEnabled\":true,\"webUsername\":\"{}\",\"webPassword\":\"{}\"}}",
        setupCode, apPassword, otaPassword, WEB_AUTH_USERNAME, webPassword);

    if (!configManager.updateFromJson<espConfig::misc_config_t>(payload).empty() &&
        configManager.saveConfig<espConfig::misc_config_t>()) {
      pendingSetupCode = setupCode;
      ESP_LOGW(TAG, "================= FIRST BOOT: GENERATED CREDENTIALS =================");
      ESP_LOGW(TAG, "This device now has unique credentials. Write them down:");
      ESP_LOGW(TAG, "  HomeKit Setup Code : %.3s-%.2s-%.3s", setupCode.c_str(), setupCode.c_str() + 3, setupCode.c_str() + 5);
      ESP_LOGW(TAG, "  Setup AP password  : %s", apPassword.c_str());
      ESP_LOGW(TAG, "  Web UI login       : %s / %s", WEB_AUTH_USERNAME, webPassword.c_str());
      ESP_LOGW(TAG, "  OTA password       : %s", otaPassword.c_str());
      ESP_LOGW(TAG, "The Web UI password can be changed under Misc -> Security.");
      ESP_LOGW(TAG, "=====================================================================");
      return;
    }
    ESP_LOGE(TAG, "Failed to store the generated credentials; falling back to the compiled defaults.");
  }

  // Already-configured device: report anything that is still on a shipped default
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

std::function<void(int)> lambda = [](int status) {
  if (status == 1) {
    char identifier[18];
    sprintf(identifier, "%.2s%.2s%.2s%.2s%.2s%.2s", HAPClient::accessory.ID, HAPClient::accessory.ID + 3, HAPClient::accessory.ID + 6, HAPClient::accessory.ID + 9, HAPClient::accessory.ID + 12, HAPClient::accessory.ID + 15);
    mqttManager->begin(std::string(identifier));
    webServerManager.begin(); 
  } else if (status == 0){
    pollHS = false;
    mqttManager->end();
    webServerManager.end();
    WiFi.mode(WIFI_AP_STA);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    const std::string macStr = fmt::format("HK_{:02X}{:02X}{:02X}{:02X}", mac[2], mac[3], mac[4], mac[5]);
    auto misc = configManager.getConfig<espConfig::misc_config_t>();
    WiFi.softAP(macStr.c_str(), misc.accessPointPassword.c_str(), 11, false, 2, false, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_CIPHER_TYPE_AES_CMAC128); 
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
        WiFi.softAP(macStr.c_str(), configManager.getConfig<espConfig::misc_config_t>().accessPointPassword.c_str(), 11, false, 2, false, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_CIPHER_TYPE_AES_CMAC128);
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
  hardwareManager = std::make_unique<HardwareManager>(configManager.getConfig<espConfig::actions_config_t>());
  lockManager = std::make_unique<LockManager>(configManager.getConfig<espConfig::misc_config_t>(), configManager.getConfig<espConfig::actions_config_t>());
  mqttManager = std::make_unique<MqttManager>(configManager);
  homekitLock = std::make_unique<HomeKitLock>(lambda, *lockManager, configManager, readerDataManager);
  espConfig::misc_config_t miscConfig = configManager.getConfig<espConfig::misc_config_t>();
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
  hardwareManager->begin();
  homekitLock->begin();
  // HomeSpan keeps the SRP verification data for the Setup Code in its own NVS
  // namespace, so a code generated on first boot has to be handed over once
  // HomeSpan is running. Doing it here also means it happens exactly once instead
  // of regenerating SRP data on every boot.
  if (!pendingSetupCode.empty()) {
    ESP_LOGI("Main", "Applying the generated HomeKit Setup Code.");
    homeSpan.setPairingCode(pendingSetupCode.c_str(), false);
    pendingSetupCode.clear();
  }
  lockManager->begin();
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
  vTaskDelay(pdMS_TO_TICKS(50));
}
