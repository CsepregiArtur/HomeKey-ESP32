#include "fmt/ranges.h"
#include "config.hpp"
#include "MqttManager.hpp"
#include "json_escape.hpp"
#include "LockManager.hpp"
#include "ConfigManager.hpp"
#include "JsonGuard.hpp"
#include "HouseholdManager.hpp"
#include "NodeIdentityManager.hpp"
#include "HealthManager.hpp"
#include "AuditManager.hpp"
#include "ReaderDataManager.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_timer.h>
#include <sodium.h>
#include <cJSON.h>
#include <ctime>
#include "eventStructs.hpp"
#include <string>
#include <vector>

const char* MqttManager::TAG = "MqttManager";

namespace {
/// Wall-clock seconds when available, falling back to monotonic uptime (same
/// convention as BackupManager). Used only for safe telemetry timestamps.
uint64_t wallClockSeconds() {
    const time_t t = time(nullptr);
    if (t > 1000000000) {
        return static_cast<uint64_t>(t);
    }
    return static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
}
} // namespace

/**
 * @brief Initialize MqttManager from configuration and register MQTT-related event subscribers and publishers.
 *
 * Constructs the manager using settings from the provided ConfigManager, initializes internal MQTT configuration
 * and device name, and registers event subscribers and publishers used to bridge internal events (lock state,
 * alternate lock actions, and NFC/Home Key events) to MQTT publishing and vice versa.
 *
 * @param configManager Source of runtime configuration values used to initialize the MQTT manager.
 */
MqttManager::MqttManager(const ConfigManager& configManager)
    : m_mqttConfig(configManager.getConfig<espConfig::mqttConfig_t>()),
      m_mqttSslConfig(configManager.getMqttSslConfig()),
      m_client(nullptr),
      device_name(configManager.getConfig<espConfig::misc_config_t>().deviceName),
      m_sslConfigured(false)
{
}

/**
 * @brief Stops and destroys the MQTT client (if active) and unsubscribes all EventBus listeners registered by this instance.
 *
 * Ensures the MQTT client is cleanly stopped and its resources freed, then removes subscriptions for lock state, alternate action, and NFC events from the shared EventBus.
 */
MqttManager::~MqttManager() {
   if (m_client) {
       esp_mqtt_client_stop(m_client);
       esp_mqtt_client_destroy(m_client);
       m_client = nullptr;
   }
}

/**
 * @brief Stops the MQTT client and unsubscribes from all EventBus listeners.
 *
 * Performs a clean shutdown of the MQTT client by stopping and destroying it,
 * then removes all EventBus subscriptions registered by this instance.
 */
void MqttManager::end() {
    if (m_client) {
        ESP_LOGI(TAG, "Stopping MQTT client...");
        esp_mqtt_client_stop(m_client);
        esp_mqtt_client_destroy(m_client);
        m_client = nullptr;
        m_isConnected = false;
        ESP_LOGI(TAG, "MQTT client stopped");
    }
}

/**
 * @brief Configure and start the MQTT client using the manager's stored configuration and a device identifier.
 *
 * Initializes the MQTT client configuration (including optional SSL/TLS), registers the instance event handler,
 * subscribes to internal EventBus topics for lock and NFC events, and starts the MQTT client.
 *
 * @param deviceID Unique device identifier used for MQTT client identification and discovery topics.
 * @return true if the MQTT client was started successfully, false otherwise.
 */
bool MqttManager::begin(std::string deviceID) {
    if (m_mqttConfig.mqttBroker.empty() || m_mqttConfig.mqttBroker == "0.0.0.0") {
        ESP_LOGW(TAG, "MQTT broker host is not configured. MQTT client will not start.");
        return false;
    }

    m_lock_state_changed = AppEventLoop::subscribe(LOCK_EVENT, LOCK_STATE_CHANGED, [&](const uint8_t* data, size_t size){
      if(size == 0 || data == nullptr) return;
      std::span<const uint8_t> payload(data, size);
      std::error_code ec;
      EventLockState s = alpaca::deserialize<EventLockState>(payload, ec);
      if(ec) { ESP_LOGE(TAG, "Failed to deserialize lock state event: %s", ec.message().c_str()); return; }
      ESP_LOGD(TAG, "Received lock state event: %d -> %d", s.currentState, s.targetState);
      // Published before the state itself, so a subscriber always has the cause on hand
      // by the time the state it explains arrives.
      if(!ec) publishLockChange(s.currentState, s.targetState, s.source);
      if(!ec) publishLockState(s.currentState, s.targetState);
    });
    m_alt_action = AppEventLoop::subscribe(HW_EVENT, HW_ALT_ACTION, [&](const uint8_t* data, size_t size){
      (void)data; (void)size;
      publish(m_mqttConfig.hkAltActionTopic, "1");
    });
    m_nfc_event = AppEventLoop::subscribe(NFC_EVENT, NFC_TAP_EVENT, [&](const uint8_t* data, size_t size){
      if(size == 0 || data == nullptr) return;
      std::span<const uint8_t> payload(data, size);
      std::error_code ec;
      NfcEvent nfc_event = alpaca::deserialize<NfcEvent>(payload, ec);
      if(ec) { ESP_LOGE(TAG, "Failed to deserialize NFC event: %s", ec.message().c_str()); return; }
      switch(nfc_event.type) {
        case HOMEKEY_TAP: {
          EventHKTap s = alpaca::deserialize<EventHKTap>(nfc_event.data, ec);
          if(!ec){
            if(s.status){
              publishHomeKeyTap(s.issuerId, s.endpointId, s.readerId);
            }
            // The name the user gave this controller, if any. Looked up here because this
            // is the one place that knows which issuer just authenticated: the lock event
            // that follows carries only how the change was requested, not by whom.
            const std::string issuerLabel =
                m_readerData == nullptr ? std::string() : m_readerData->issuerLabel(s.issuerId);
            publishLastAuth("HomeKey", s.status ? "SUCCESS" : "FAILURE", issuerLabel);
          } else {
            ESP_LOGE(TAG, "Failed to deserialize HomeKey event: %s", ec.message().c_str());
            return;
          }
        }
        break;
        case TAG_TAP: {
          EventTagTap s = alpaca::deserialize<EventTagTap>(nfc_event.data, ec);
          if(!ec){
            publishUidTap(s.uid, s.atqa, s.sak);
          } else {
            ESP_LOGE(TAG, "Failed to deserialize Tag event: %s", ec.message().c_str());
            return;
          }
        }
        break;
        default:
          break;
      }
    });
    this->deviceID = deviceID;

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname = m_mqttConfig.mqttBroker.c_str();
    mqtt_cfg.broker.address.port = m_mqttConfig.mqttPort;
    
    if (m_mqttConfig.useSSL) {
        ESP_LOGI(TAG, "SSL/TLS is enabled for MQTT connection");
        mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
        
        if (m_mqttConfig.allowInsecure) {
            ESP_LOGW(TAG, "Security warning: SSL/TLS is enabled but certificate validation is disabled");
        }
        if (!configureSSL(mqtt_cfg)) {
            ESP_LOGE(TAG, "Failed to configure SSL/TLS for MQTT connection");
            return false;
        }
    } else {
        mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
        // Worth repeating on every connect: the MQTT topics below can drive the lock,
        // so without TLS both the broker credentials and the unlock commands are
        // readable and forgeable by anything on the path.
        ESP_LOGW(TAG, "MQTT TLS is disabled: credentials and lock commands are sent in the "
                      "clear. Enable SSL/TLS and validate the broker certificate if the "
                      "broker is reachable from anywhere but a trusted LAN.");
    }
    
    mqtt_cfg.credentials.client_id = m_mqttConfig.mqttClientId.c_str();
    mqtt_cfg.credentials.username = m_mqttConfig.mqttUsername.c_str();
    mqtt_cfg.credentials.authentication.password = m_mqttConfig.mqttPassword.c_str();
    mqtt_cfg.session.last_will.topic = m_mqttConfig.lwtTopic.c_str();
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.msg_len = 7;
    mqtt_cfg.session.last_will.retain = true;
    mqtt_cfg.session.last_will.qos = 1;

    m_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!m_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return false;
    }
    
    esp_mqtt_client_register_event(m_client, MQTT_EVENT_ANY, mqttEventHandler, this);
    esp_err_t start_result = esp_mqtt_client_start(m_client);
    
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(start_result));
        esp_mqtt_client_destroy(m_client);
        m_client = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "MQTT client started successfully%s", m_mqttConfig.useSSL ? " with SSL/TLS" : "");
    return true;
}

/**
 * @brief Publish a message to the configured MQTT broker.
 *
 * Publishes the given payload to the specified MQTT topic using the provided QoS and retain flag.
 *
 * @param topic Destination MQTT topic.
 * @param payload Message payload (may be empty).
 * @param qos MQTT quality of service level (typically 0, 1, or 2).
 * @param retain If true, the broker will retain the message as the last known value for the topic.
 */
void MqttManager::publish(const std::string& topic, const std::string& payload, int qos, bool retain) {
    if (!m_client) {
        ESP_LOGW(TAG, "Cannot publish, MQTT client not initialized.");
        return;
    }
    esp_mqtt_client_publish(m_client, topic.c_str(), payload.c_str(), payload.length(), qos, retain);
}

/**
 * @brief MQTT event callback that forwards received events to the associated MqttManager instance.
 *
 * Casts @p handler_args to a MqttManager pointer and invokes the instance's onMqttEvent method with
 * the provided MQTT event parameters.
 *
 * @param handler_args Pointer to the MqttManager instance (passed by the MQTT library).
 * @param base MQTT event base identifier.
 * @param event_id Numeric MQTT event identifier.
 * @param event_data Pointer to event-specific data provided by the MQTT library.
 */

void MqttManager::mqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    MqttManager* instance = static_cast<MqttManager*>(handler_args);
    instance->onMqttEvent(base, event_id, event_data);
}

/**
 * @brief Dispatches incoming MQTT events to the appropriate handlers and logs status.
 *
 * Interprets the MQTT event contained in `event_data` and:
 * - invokes onConnected() when a connection is established,
 * - logs disconnection and errors,
 * - extracts topic and payload and forwards them to onData() for incoming messages,
 * - logs unhandled event types.
 *
 * @param base MQTT event base (unused by this method).
 * @param event_id Raw event identifier (unused; the method reads the id from `event_data`).
 * @param event_data Pointer to an `esp_mqtt_event_t`-compatible structure containing the MQTT event, topic, and payload.
 */
void MqttManager::onMqttEvent(esp_event_base_t base, int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    ESP_LOGD(TAG, "MQTT Event received: %d", event->event_id);

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED: Connection established successfully");
            m_isConnected = true;
            publishMqttStatus(true, MqttErrorCode::NONE);
            onConnected();
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED: Client disconnected from broker");
            m_isConnected = false;
            publishMqttStatus(false, MqttErrorCode::NONE);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED: Successfully subscribed to topic (msg_id=%d)", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED: Successfully unsubscribed from topic (msg_id=%d)", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED: Message published successfully (msg_id=%d)", event->msg_id);
            break;
        case MQTT_EVENT_DATA: {
            std::string topic(event->topic, event->topic_len);
            std::string data(event->data, event->data_len);
            ESP_LOGD(TAG, "MQTT_EVENT_DATA: Received %d bytes on topic '%s'", event->data_len, topic.c_str());
            onData(topic, data);
            break;
        }
        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(TAG, "MQTT_EVENT_BEFORE_CONNECT: Initiating connection attempt to %s:%d",
                     m_mqttConfig.mqttBroker.c_str(), m_mqttConfig.mqttPort);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR: Connection or protocol error occurred");
            {
                MqttErrorCode errCode = MqttErrorCode::UNKNOWN;
                std::string errMsg;
                if (event->error_handle) {
                    ESP_LOGE(TAG, "MQTT Error Type: %d", event->error_handle->error_type);

                    // Categorize and handle different error types with detailed logging
                    switch (event->error_handle->error_type) {
                        case MQTT_ERROR_TYPE_TCP_TRANSPORT:
                            errCode = MqttErrorCode::NETWORK_ERROR;
                            if (event->error_handle->esp_transport_sock_errno != 0) {
                                errMsg = "Transport socket error";
                                ESP_LOGE(TAG, "Transport socket error: errno=%d (%s)",
                                         event->error_handle->esp_transport_sock_errno,
                                         strerror(event->error_handle->esp_transport_sock_errno));
                            }
                            break;
                        case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
                            errCode = MqttErrorCode::CONNECTION_REFUSED;
                            errMsg = "Connection refused";
                            ESP_LOGE(TAG, "Connection refused - broker may be down or rejecting connection");
                            break;
                        case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:
                            errCode = MqttErrorCode::AUTH_FAILED;
                            errMsg = "Subscribe failed";
                            ESP_LOGE(TAG, "Subscribe failed - check topic permissions");
                            break;
                        default:
                            errCode = MqttErrorCode::UNKNOWN;
                            errMsg = "Unknown error";
                            ESP_LOGE(TAG, "Unknown MQTT error type: %d", event->error_handle->error_type);
                            break;
                    }

                if (event->error_handle->esp_tls_last_esp_err != 0) {
                    logSSLError("MQTT SSL/TLS connection", event->error_handle->esp_tls_last_esp_err);
                }

                if (event->error_handle->esp_tls_stack_err != 0) {
                    ESP_LOGE(TAG, "TLS stack error: 0x%x", event->error_handle->esp_tls_stack_err);
                }

                ESP_LOGI(TAG, "Connection attempt details: broker=%s:%d, client_id=%s, ssl=%s",
                         m_mqttConfig.mqttBroker.c_str(), m_mqttConfig.mqttPort,
                         m_mqttConfig.mqttClientId.c_str(), m_mqttConfig.useSSL ? "enabled" : "disabled");
                    publishMqttStatus(false, errCode, errMsg);
                } else {
                    ESP_LOGE(TAG, "MQTT error occurred but no error handle available");
                    publishMqttStatus(false, MqttErrorCode::UNKNOWN, "No error details");
                }
            }
            break;
        default:
            ESP_LOGD(TAG, "Unhandled MQTT event: %d", event->event_id);
            break;
    }
}

/**
 * @brief Handle actions to perform immediately after the MQTT client connects.
 *
 * Publishes the configured "online" last-will/retain presence message, subscribes to configured command topics used for lock and battery commands, and triggers Home Assistant MQTT discovery if enabled.
 *
 * The retained presence message is published to the configured LWT topic with QoS 1. Subscriptions include lock state/command topics and the battery level command topic; the custom lock state command topic is subscribed only when custom states are enabled. If Home Assistant discovery is enabled in configuration, discovery payloads are published.
 */
void MqttManager::onConnected() {
    m_isConnected = true;

    publish(m_mqttConfig.lwtTopic, "online", 1, true);

    int ret;
    ret = esp_mqtt_client_subscribe(m_client, m_mqttConfig.lockStateCmd.c_str(), 0);
    if (ret < 0) ESP_LOGW(TAG, "Failed to subscribe to lockStateCmd");
    ret = esp_mqtt_client_subscribe(m_client, m_mqttConfig.lockCStateCmd.c_str(), 0);
    if (ret < 0) ESP_LOGW(TAG, "Failed to subscribe to lockCStateCmd");
    ret = esp_mqtt_client_subscribe(m_client, m_mqttConfig.lockTStateCmd.c_str(), 0);
    if (ret < 0) ESP_LOGW(TAG, "Failed to subscribe to lockTStateCmd");
    ret = esp_mqtt_client_subscribe(m_client, m_mqttConfig.btrLvlCmdTopic.c_str(), 0);
    if (ret < 0) ESP_LOGW(TAG, "Failed to subscribe to btrLvlCmdTopic");
    if (m_mqttConfig.lockEnableCustomState) {
        ret = esp_mqtt_client_subscribe(m_client, m_mqttConfig.lockCustomStateCmd.c_str(), 0);
        if (ret < 0) ESP_LOGW(TAG, "Failed to subscribe to lockCustomStateCmd");
    }

    // Household/node authenticated command topics (only when enrolled).
    const std::string base = baseTopic();
    if (!base.empty()) {
        const std::string cmdLock = base + "/command/lock";
        const std::string cmdUnlock = base + "/command/unlock";
        ret = esp_mqtt_client_subscribe(m_client, cmdLock.c_str(), 1);
        if (ret < 0) ESP_LOGW(TAG, "Failed to subscribe to command/lock");
        ret = esp_mqtt_client_subscribe(m_client, cmdUnlock.c_str(), 1);
        if (ret < 0) ESP_LOGW(TAG, "Failed to subscribe to command/unlock");
    }

    if (m_mqttConfig.hassMqttDiscoveryEnabled) {
        publishHassDiscovery();
    }

    // Node telemetry is independent of HASS discovery; publish it immediately on
    // connect so household entities have state before the next periodic update.
    publishNodeStatus();
}

/**
 * @brief Translate received MQTT command messages into internal EventBus events.
 *
 * Processes configured MQTT command topics (lock state/target/current/custom and battery level)
 * and publishes the corresponding EventLockState or HomeKit events onto the EventBus.
 *
 * @param topic MQTT topic of the received message (used to determine the command).
 * @param data Payload of the received message as a string (interpreted per-topic).
 */
void MqttManager::onData(const std::string& topic, const std::string& data) {
    ESP_LOGI(TAG, "Received message on topic '%s': %s", topic.c_str(), data.c_str());

    // Authenticated household command namespace is handled separately; it is
    // never processed through the legacy numeric topic path below.
    if (handleSecureCommand(topic, data)) {
        return;
    }

    auto to_u8 = [](const std::string &str, uint8_t& out) -> bool {
      const char* begin = str.c_str(); char* end = nullptr;
      unsigned long v = strtoul(begin, &end, 10);
      if(end == begin || v > 255) return false;
      out = static_cast<uint8_t>(v); return true;
    };
    EventLockState s{
    .source = LockManager::MQTT
    };
    std::array<uint8_t, sizeof(EventLockState)> d{};
    if (topic == m_mqttConfig.lockStateCmd) {
      uint8_t v; if (!to_u8(data, v)) { ESP_LOGW(TAG, "Invalid lockStateCmd payload: %s", data.c_str()); return; }
      s.currentState = v;
      s.targetState = v;
      size_t d_len = alpaca::serialize(s, d);
      AppEventLoop::publish(LOCK_EVENT, LOCK_OVERRIDE_STATE, d.data(), d_len);
    } else if (topic == m_mqttConfig.lockTStateCmd) {
      uint8_t v; if (!to_u8(data, v)) { ESP_LOGW(TAG, "Invalid lockTStateCmd payload: %s", data.c_str()); return; }
      s.currentState = LockManager::UNKNOWN;
      s.targetState = v;
      size_t d_len = alpaca::serialize(s, d);
      AppEventLoop::publish(LOCK_EVENT, LOCK_TARGET_STATE_CHANGED, d.data(), d_len);
    } else if (topic == m_mqttConfig.lockCStateCmd) {
      uint8_t v; if (!to_u8(data, v)) { ESP_LOGW(TAG, "Invalid lockCStateCmd payload: %s", data.c_str()); return; }
      s.currentState = v;
      s.targetState = LockManager::UNKNOWN;
      size_t d_len = alpaca::serialize(s, d);
      AppEventLoop::publish(LOCK_EVENT, LOCK_UPDATE_STATE, d.data(), d_len);
    } else if (m_mqttConfig.lockEnableCustomState &&
               topic == m_mqttConfig.lockCustomStateCmd) {
      uint8_t v; if (!to_u8(data, v)) { ESP_LOGW(TAG, "Invalid lockCStateCmd payload: %s", data.c_str()); return; }
      if (m_mqttConfig.customLockStates.at("C_UNLOCKING") == v) {
        s.currentState = LockManager::MAX;
        s.targetState = LockManager::UNLOCKED;
        size_t d_len = alpaca::serialize(s, d);
        AppEventLoop::publish(LOCK_EVENT, LOCK_TARGET_STATE_CHANGED, d.data(), d_len);
      } else if (m_mqttConfig.customLockStates.at("C_LOCKING") == v) {
        s.currentState = LockManager::MAX;
        s.targetState = LockManager::LOCKED;
        size_t d_len = alpaca::serialize(s, d);
        AppEventLoop::publish(LOCK_EVENT, LOCK_TARGET_STATE_CHANGED, d.data(), d_len);
      } else if (m_mqttConfig.customLockStates.at("C_UNLOCKED") == v) {
        s.currentState = LockManager::UNLOCKED;
        s.targetState = LockManager::UNLOCKED;
        size_t d_len = alpaca::serialize(s, d);
        AppEventLoop::publish(LOCK_EVENT, LOCK_OVERRIDE_STATE, d.data(), d_len);
      } else if (m_mqttConfig.customLockStates.at("C_LOCKED") == v) {
        s.currentState = LockManager::LOCKED;
        s.targetState = LockManager::LOCKED;
        size_t d_len = alpaca::serialize(s, d);
        AppEventLoop::publish(LOCK_EVENT, LOCK_OVERRIDE_STATE, d.data(), d_len);
      } else if (m_mqttConfig.customLockStates.at("C_JAMMED") == v) {
        s.currentState = LockManager::JAMMED;
        s.targetState = LockManager::MAX;
        size_t d_len = alpaca::serialize(s, d);
        AppEventLoop::publish(LOCK_EVENT, LOCK_OVERRIDE_STATE, d.data(), d_len);
      } else if (m_mqttConfig.customLockStates.at("C_UNKNOWN") == v) {
        s.currentState = LockManager::UNKNOWN;
        s.targetState = LockManager::MAX;
        size_t d_len = alpaca::serialize(s, d);
        AppEventLoop::publish(LOCK_EVENT, LOCK_OVERRIDE_STATE, d.data(), d_len);
      }
    } else if (topic == m_mqttConfig.btrLvlCmdTopic) { 
        uint8_t v; if (!to_u8(data, v)) { ESP_LOGW(TAG, "Invalid btrLvlCmdTopic payload: %s", data.c_str()); return; }
        EventValueChanged s{
          .name = "btrLevel",
          .oldValue = 0,
          .newValue = v,
        };
        std::vector<uint8_t> d;
        alpaca::serialize(s, d);
        HomekitEvent event{.type = HomekitEventType::BTR_PROP_CHANGED, .data = d};
        std::vector<uint8_t> event_data;
        alpaca::serialize(event, event_data);
        AppEventLoop::publish(HK_EVENT, HK_INTERNAL_EVENT, event_data.data(), event_data.size());
    }
}

/**
 * @brief Publish the lock's current/target state to the configured MQTT state topic.
 *
 * Publishes a string representation of the lock state to the MQTT topic configured in m_mqttConfig.lockStateTopic.
 * If the current state differs from the target state, the published value indicates an in-transition state
 * ("locking" or "unlocking"); otherwise the numeric current state is published. The message is sent with QoS 0
 * and retained.
 *
 * @param currentState Numeric code representing the lock's current state.
 * @param targetState Numeric code representing the lock's target state.
 */

void MqttManager::publishLockChange(const int currentState, const int targetState,
                                    const uint8_t source) {
    const std::string base = baseTopic();
    if (base.empty()) {
        return;
    }
    // Retained: the point of this topic is to answer "who did that?" for the change
    // that produced the state the client is looking at, and a client that connects
    // afterwards still needs the last answer. No secrets are involved - a source is
    // one of five fixed words.
    publish(base + "/lock/last",
            fmt::format("{{\"current\":{},\"target\":{},\"source\":\"{}\",\"timestamp\":{}}}",
                        currentState, targetState, LockManager::sourceName(source),
                        wallClockSeconds()),
            0, true);
}

void MqttManager::publishLockState(const int currentState, const int targetState) {
    std::string stateStr;
    if (currentState != targetState) {
        stateStr = (targetState == LockManager::UNLOCKED) ? std::to_string(LockManager::UNLOCKING) : std::to_string(LockManager::LOCKING);
    } else {
        stateStr = std::to_string(currentState);
    }
    publish(m_mqttConfig.lockStateTopic, stateStr, 0, true);
    if(m_mqttConfig.lockEnableCustomState){
      publish(m_mqttConfig.lockCustomStateTopic, (targetState == LockManager::UNLOCKED) ? std::to_string(m_mqttConfig.customLockActions.at("UNLOCK")) : std::to_string(m_mqttConfig.customLockActions.at("LOCK")));
    }
}

/**
 * @brief Publish a Home Key Tap event to the configured Home Key MQTT topic.
 *
 * Constructs a JSON payload containing hex-encoded identifiers and a "homekey" flag, then publishes it to the configured Home Key topic.
 *
 * @param issuerId Byte sequence of the issuer identifier; encoded as an uppercase hex string in the `issuerId` JSON field.
 * @param endpointId Byte sequence of the endpoint identifier; encoded as an uppercase hex string in the `endpointId` JSON field.
 * @param readerId Byte sequence of the reader identifier; encoded as an uppercase hex string in the `readerId` JSON field.
 */
void MqttManager::publishHomeKeyTap(const std::vector<uint8_t>& issuerId, const std::vector<uint8_t>& endpointId, const std::vector<uint8_t>& readerId) {
    std::string payload = JsonBuilder::object()
        .addString("issuerId", fmt::format("{:02X}", fmt::join(issuerId, "")))
        .addString("endpointId", fmt::format("{:02X}", fmt::join(endpointId, "")))
        .addString("readerId", fmt::format("{:02X}", fmt::join(readerId, "")))
        .addBool("homekey", true)
        .toStringUnformatted();
    publish(m_mqttConfig.hkTopic, payload);
}

/**
 * @brief Publish an NFC tag UID tap payload to the configured Home Key topic.
 *
 * When configured to allow NFC tag publishing, builds a JSON payload containing
 * the tag UID, ATQA, and SAK as uppercase hex strings and a `homekey` flag set
 * to `false`, then publishes it to the manager's configured hkTopic.
 *
 * @param uid Byte vector of the tag UID to include in the payload.
 * @param atqa Byte vector of the tag ATQA to include in the payload.
 * @param sak Byte vector of the tag SAK to include in the payload.
 *
 * If NFC tag publishing is disabled in the MQTT configuration, no publish is performed.
 */
void MqttManager::publishUidTap(const std::vector<uint8_t>& uid, const std::array<uint8_t,2> &atqa, const uint8_t &sak) {
    if(!m_mqttConfig.nfcTagNoPublish){
      std::string payload = JsonBuilder::object()
          .addString("uid", fmt::format("{:02X}", fmt::join(uid, "")))
          .addBool("homekey", false)
          .addString("atqa", fmt::format("{:02X}", fmt::join(atqa, "")))
          .addString("sak", fmt::format("{:02X}", sak))
          .addString("readerId", this->deviceID)
          .toStringUnformatted();
      publish(m_mqttConfig.hkTopic, payload);
    } else ESP_LOGW(TAG, "MQTT publishing of Tag UID not enabled, ignoring!");
}

/**
 * @brief Publish Home Assistant MQTT discovery payloads for the device's lock and (optionally) NFC tag.
 *
 * Composes a device descriptor and sends retained, QoS 1 discovery messages to Home Assistant discovery topics
 * so the lock entity and, if enabled, an NFC tag entity are automatically discovered. The lock payload includes
 * state and command topics, payload values for lock states, and the availability topic. The NFC/tag payload
 * includes the topic and a JSON value template for extracting the tag UID.
 */
void MqttManager::publishHassDiscovery() {
    ESP_LOGI(TAG, "Publishing Home Assistant discovery messages...");

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    std::string macStr = fmt::format("HK-{:02X}{:02X}{:02X}{:02X}", mac[2], mac[3], mac[4], mac[5]);

    JsonBuilder device = JsonBuilder::object();
    if (!device) {
        ESP_LOGE(TAG, "Failed to allocate device JSON object (OOM)");
        return;
    }

    JsonBuilder identifiers = JsonBuilder::array();
    if (identifiers) {
        identifiers.addItemToArray(JsonGuard(cJSON_CreateString(deviceID.c_str())));
        identifiers.addItemToArray(JsonGuard(cJSON_CreateString(macStr.c_str())));
    }
    device.addItem("identifiers", JsonGuard(identifiers.release()));
    
    device.addString("name", device_name.c_str());
    device.addString("manufacturer", "rednblkx");
    device.addString("model", "HomeKey-ESP32");
    device.addString("sw_version", esp_app_get_description()->version);
    device.addString("configuration_url", fmt::format("http://{}.local", macStr).c_str());
    device.addString("serial_number", macStr.c_str());

    std::string lockedStr = std::to_string(LockManager::LOCKED);
    std::string unlockedStr = std::to_string(LockManager::UNLOCKED);
    std::string lockingStr = std::to_string(LockManager::LOCKING);
    std::string unlockingStr = std::to_string(LockManager::UNLOCKING);
    std::string jammedStr = std::to_string(LockManager::JAMMED);

    auto publishConfig = [&](const char* name, const std::string& topicSuffix, auto fillPayload) {
        JsonBuilder payload = JsonBuilder::object();
        if (!payload) {
            ESP_LOGE(TAG, "Failed to allocate payload JSON object (OOM)");
            return;
        }

        payload.addString("name", name);
        payload.addString("unique_id", deviceID.c_str());
        payload.addItem("device", JsonGuard(cJSON_Duplicate(device.get(), true)));

        fillPayload(payload);

        std::string payloadStr = payload.toStringFormatted();
        std::string topic = "homeassistant/" + topicSuffix;
        publish(topic, payloadStr, 1, true);
    };

    // Publish Lock config
    publishConfig("Lock", "lock/" + m_mqttConfig.mqttClientId + "/lock/config", [&](JsonBuilder& p) {
        p.addString("state_topic", m_mqttConfig.lockStateTopic.c_str());
        p.addString("command_topic", m_mqttConfig.lockTStateCmd.c_str());
        p.addString("payload_lock", lockedStr.c_str());
        p.addString("payload_unlock", unlockedStr.c_str());
        p.addString("state_locked", lockedStr.c_str());
        p.addString("state_unlocked", unlockedStr.c_str());
        p.addString("state_locking", lockingStr.c_str());
        p.addString("state_unlocking", unlockingStr.c_str());
        p.addString("state_jammed", jammedStr.c_str());
        p.addString("availability_topic", m_mqttConfig.lwtTopic.c_str());
    });

    // Publish HomeKey Issuer config
    publishConfig("HomeKey Issuer", "tag/" + m_mqttConfig.mqttClientId + "/hk_issuer/config", [&](JsonBuilder& p) {
        p.addString("topic", m_mqttConfig.hkTopic.c_str());
        p.addString("value_template", "{{ value_json.issuerId }}");
    });

    // Publish HomeKey Endpoint config
    publishConfig("HomeKey Endpoint", "tag/" + m_mqttConfig.mqttClientId + "/hk_endpoint/config", [&](JsonBuilder& p) {
        p.addString("topic", m_mqttConfig.hkTopic.c_str());
        p.addString("value_template", "{{ value_json.endpointId }}");
    });

    // Publish NFC Tag config (conditional)
    if (!m_mqttConfig.nfcTagNoPublish) {
        publishConfig("NFC Tag", "tag/" + m_mqttConfig.mqttClientId + "/rfid/config", [&](JsonBuilder& p) {
            p.addString("topic", m_mqttConfig.hkTopic.c_str());
            p.addString("value_template", "{{ value_json.uid }}");
        });
    }

    // Household/node entities (only when enrolled). Stable unique id:
    // <household_id>_<node_id>_<entity>.
    const std::string base = baseTopic();
    if (!base.empty() && m_household && m_node) {
        const std::string nodeUid = m_household->info().household_id + "_" + m_node->info().node_id;

        // Every household entity MUST get its own discovery topic (object id), or
        // Home Assistant keeps only the last config published on a shared topic.
        // Convention: discovery object id == unique-id suffix ==
        // "<hid>_<nid>_<entity>", except the node-online binary_sensor which keeps
        // the bare "<hid>_<nid>" object id. All ids are stable across reconnect and
        // reboot (retained configs; deterministic construction from household/node).
        auto publishNodeConfig = [&](const char* name, const std::string& component,
                                     const std::string& objectId, const std::string& entityId,
                                     auto fillPayload) {
            JsonBuilder payload = JsonBuilder::object();
            if (!payload) {
                ESP_LOGE(TAG, "Failed to allocate node discovery JSON object (OOM)");
                return;
            }
            payload.addString("name", name);
            payload.addString("unique_id", (nodeUid + "_" + entityId).c_str());
            payload.addItem("device", JsonGuard(cJSON_Duplicate(device.get(), true)));
            fillPayload(payload);
            publish("homeassistant/" + component + "/" + objectId + "/config",
                    payload.toStringFormatted(), 1, true);
        };

        publishNodeConfig("Node online", "binary_sensor", nodeUid, "online", [&](JsonBuilder& p) {
            p.addString("state_topic", (base + "/status").c_str());
            p.addString("payload_on", "online");
            p.addString("payload_off", "offline");
            // MQTT allows a single will per connection, and the ESP-IDF client's
            // will is already configured on the legacy availability topic. The
            // household node presence therefore reuses that broker LWT via
            // availability_topic (additive; the legacy behaviour is untouched), so
            // the entity goes unavailable on an unexpected disconnect instead of
            // staying permanently online.
            p.addString("availability_topic", m_mqttConfig.lwtTopic.c_str());
            p.addString("payload_available", "online");
            p.addString("payload_not_available", "offline");
        });
        publishNodeConfig("Node health", "sensor", nodeUid + "_health", "health", [&](JsonBuilder& p) {
            p.addString("state_topic", (base + "/health").c_str());
            p.addString("value_template", "{{ value_json.mqtt }}");
        });

        // Household entities with stable, self-describing unique ids:
        // <household_id>_<node_id>_<entity>.
        publishNodeConfig("Backup status", "sensor", nodeUid + "_backup", "backup", [&](JsonBuilder& p) {
            p.addString("state_topic", (base + "/backup/last").c_str());
            p.addString("value_template", "{{ value_json.status }}");
            p.addString("json_attributes_topic", (base + "/backup/last").c_str());
            p.addString("json_attributes_template", "{{ value_json | tojson }}");
        });
        publishNodeConfig("Security status", "sensor", nodeUid + "_security", "security", [&](JsonBuilder& p) {
            p.addString("state_topic", (base + "/security").c_str());
        });
        publishNodeConfig("Firmware version", "sensor", nodeUid + "_firmware", "firmware", [&](JsonBuilder& p) {
            p.addString("state_topic", (base + "/state").c_str());
            p.addString("value_template", "{{ value_json.firmware_version }}");
        });
        publishNodeConfig("Last HomeKey authentication", "sensor", nodeUid + "_last_auth", "last_auth", [&](JsonBuilder& p) {
            p.addString("state_topic", (base + "/last_auth").c_str());
            p.addString("value_template", "{{ value_json.result }}");
            p.addString("json_attributes_topic", (base + "/last_auth").c_str());
            p.addString("json_attributes_template", "{{ value_json | tojson }}");
        });
    }

    ESP_LOGI(TAG, "HASS discovery messages published.");
}

// --- SSL/TLS Configuration Methods ---

/**
 * @brief Configure SSL/TLS settings for MQTT connection
 *
 * This method sets up SSL/TLS parameters for the MQTT client using pre-validated certificates
 * from ConfigManager. Certificate validation and loading is now centralized in ConfigManager.
 *
 * @param mqtt_cfg Reference to MQTT client configuration structure to be updated
 * @return true if SSL configuration was successful, false otherwise
 */
bool MqttManager::configureSSL(esp_mqtt_client_config_t& mqtt_cfg) {
    mqtt_cfg.broker.verification.use_global_ca_store = false;
    if (!m_mqttSslConfig.caCert.empty()) {
        mqtt_cfg.broker.verification.certificate = m_mqttSslConfig.caCert.c_str();
        mqtt_cfg.broker.verification.skip_cert_common_name_check = m_mqttConfig.allowInsecure;
        ESP_LOGI(TAG, "MQTT TLS: Certificate validation mode = %s", m_mqttConfig.allowInsecure ? "SKIP_COMMON_NAME" : "FULL_VALIDATION");
    } else {
        ESP_LOGE(TAG, "MQTT TLS: FAILED - No CA certificate provided");
        return false;
    }

    if (!m_mqttSslConfig.clientCert.empty() && !m_mqttSslConfig.clientKey.empty()) {
        mqtt_cfg.credentials.authentication.certificate = m_mqttSslConfig.clientCert.c_str();
        mqtt_cfg.credentials.authentication.key = m_mqttSslConfig.clientKey.c_str();
        ESP_LOGI(TAG, "MQTT TLS: TLS client authentication configured");
    } else {
        ESP_LOGI(TAG, "MQTT TLS: No client certificate configured - using server-only authentication");
    }

    m_sslConfigured = true;
    return true;
}


void MqttManager::logSSLError(const char* operation, esp_err_t error) {
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "SSL/TLS error during %s: %s (0x%x)", operation, esp_err_to_name(error), error);

        // Provide detailed error information and troubleshooting hints for common SSL/TLS errors
        switch (error) {
            case ESP_FAIL:
                ESP_LOGE(TAG, "SSL/TLS operation failed - general failure");
                break;
            case ESP_ERR_INVALID_ARG:
                ESP_LOGE(TAG, "Invalid SSL/TLS configuration argument");
                break;
            case ESP_ERR_NO_MEM:
                ESP_LOGE(TAG, "Insufficient memory for SSL/TLS operation");
                break;
            case ESP_ERR_INVALID_STATE:
                ESP_LOGE(TAG, "Invalid SSL/TLS state for operation");
                break;
            case ESP_ERR_INVALID_SIZE:
                ESP_LOGE(TAG, "Invalid size in SSL/TLS operation");
                break;
            default:
                ESP_LOGE(TAG, "SSL/TLS error code: 0x%x", error);
                if (error >= 0x1000 && error < 0x2000) {
                    ESP_LOGE(TAG, "This appears to be a TLS protocol error - check cipher suite compatibility");
                } else if (error >= 0x2000 && error < 0x3000) {
                    ESP_LOGE(TAG, "This appears to be a certificate validation error - verify certificate chain");
                }
                break;
        }

        // Log current SSL configuration state for debugging
        ESP_LOGI(TAG, "SSL Configuration state: useSSL=%s, allowInsecure=%s, hasCACert=%s, hasClientCert=%s, hasClientKey=%s",
                 m_mqttConfig.useSSL ? "true" : "false",
                 m_mqttConfig.allowInsecure ? "true" : "false",
                 !m_mqttSslConfig.caCert.empty() ? "true" : "false",
                 !m_mqttSslConfig.clientCert.empty() ? "true" : "false",
                 !m_mqttSslConfig.clientKey.empty() ? "true" : "false");
    }
}

bool MqttManager::isConnected() const {
    return m_isConnected;
}

void MqttManager::publishMqttStatus(bool connected, MqttErrorCode errorCode, const std::string& errorMessage) {
    m_lastErrorCode = errorCode;
    m_lastErrorMessage = errorMessage;
    ESP_LOGD(TAG, "Updated MQTT status: connected=%s, errorCode=%d", connected ? "true" : "false", static_cast<uint8_t>(errorCode));
}

// ============================================================================
// Household / node namespace + authenticated commands
// ============================================================================

std::string MqttManager::baseTopic() const {
    if (!m_household || !m_node) {
        return "";
    }
    const auto &hh = m_household->info();
    const auto &nd = m_node->info();
    if (hh.household_id.empty() || nd.node_id.empty()) {
        return "";
    }
    return "homekey/household/" + hh.household_id + "/nodes/" + nd.node_id;
}

void MqttManager::publishNodeStatus() {
    const std::string base = baseTopic();
    if (base.empty() || !m_client) {
        return;
    }
    const auto &hh = m_household->info();
    const auto &nd = m_node->info();

    // The household id, node id and node name are all things a user typed, so they are
    // escaped: one quote in any of them would make the document unparseable and take every
    // field after it - the node's state included - with it.
    std::string state = fmt::format(
        "{{\"household_id\":\"{}\",\"node_id\":\"{}\",\"node_name\":\"{}\",\"node_role\":\"{}\","
        "\"node_state\":\"{}\",\"generation\":{},\"firmware_version\":\"{}\"}}",
        jsonEscape(hh.household_id), jsonEscape(nd.node_id), jsonEscape(nd.node_name),
        household::nodeRoleToString(nd.node_role), household::nodeStateToString(nd.state),
        nd.generation, jsonEscape(esp_app_get_description()->version));
    publish(base + "/state", state, 0, true);
    publish(base + "/status", "online", 1, true);

    if (m_health) {
        const HealthManager::Snapshot snap = m_health->snapshot();
        // Retained, like every sibling topic, and QoS 1 because this is the document that
        // carries the lock's state: a subscriber that arrives between two of these would
        // otherwise have no lock state at all for up to a full cadence, which is exactly
        // what a Home Assistant restart looked like.
        publish(base + "/health", m_health->toJson(snap), 1, true);
        // Compact security state (OK / WARNING / ERROR). ERROR is reserved for
        // when the posture cannot be computed; the firmware currently emits OK or
        // WARNING only. No numeric score.
        publish(base + "/security", snap.security_all_ok ? "OK" : "WARNING", 0, true);
    }
}

void MqttManager::publishBackupStatus(const std::string &status) {
    const std::string base = baseTopic();
    if (base.empty()) {
        return;
    }
    publish(base + "/backup/status", status, 0, true);
    // Metadata-only summary (state + wall-clock timestamp). The encrypted backup
    // contents are never published to MQTT.
    publish(base + "/backup/last",
            fmt::format("{{\"status\":\"{}\",\"timestamp\":{}}}", status, wallClockSeconds()),
            0, true);
}

void MqttManager::publishLastAuth(const std::string &authType, const std::string &result,
                                  const std::string &issuerLabel) {
    const std::string base = baseTopic();
    if (base.empty()) {
        return;
    }
    // Safe metadata only: type, result, timestamp, and - when the user named the issuer -
    // that name. Built with the JSON builder rather than string formatting because the
    // label is free text the user typed: a quote or backslash in it would otherwise
    // produce a malformed document that a subscriber would reject as a payload fault.
    // The issuer id itself is never published, named or not.
    JsonBuilder payload = JsonBuilder::object();
    payload.addString("type", authType);
    payload.addString("result", result);
    payload.addNumber("timestamp", static_cast<double>(wallClockSeconds()));
    if (!issuerLabel.empty()) {
        payload.addString("issuer", issuerLabel);
    }
    publish(base + "/last_auth", payload.toStringUnformatted(), 0, true);
}

std::string MqttManager::makeCommandMac(uint64_t ts, const std::string &nonce,
                                        const std::string &reqId, const std::string &action,
                                        const std::vector<uint8_t> &key) {
    const std::string canonical = fmt::format("{}{}{}{}", ts, nonce, reqId, action);
    std::vector<uint8_t> mac(crypto_auth_hmacsha256_BYTES);
    crypto_auth_hmacsha256(mac.data(),
                           reinterpret_cast<const unsigned char *>(canonical.data()), canonical.size(),
                           key.data());
    static const char *digits = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : mac) {
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 0x0F]);
    }
    return hex;
}

bool MqttManager::handleSecureCommand(const std::string &topic, const std::string &data) {
    const std::string base = baseTopic();
    if (base.empty()) {
        return false;
    }
    const std::string cmdUnlock = base + "/command/unlock";
    const std::string cmdLock = base + "/command/lock";
    std::string action;
    if (topic == cmdUnlock) {
        action = "unlock";
    } else if (topic == cmdLock) {
        action = "lock";
    } else {
        return false;
    }

    const std::vector<uint8_t> key = m_household->deriveCommandKey();
    if (key.empty()) {
        ESP_LOGW(TAG, "No household command key; rejecting authenticated command.");
        return true; // recognized topic, but failed closed
    }

    cJSON *root = cJSON_Parse(data.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Malformed authenticated command payload.");
        return true;
    }
    const cJSON *tsNode = cJSON_GetObjectItemCaseSensitive(root, "ts");
    const cJSON *nonceNode = cJSON_GetObjectItemCaseSensitive(root, "nonce");
    const cJSON *reqNode = cJSON_GetObjectItemCaseSensitive(root, "req_id");
    const cJSON *macNode = cJSON_GetObjectItemCaseSensitive(root, "mac");
    const bool ok = cJSON_IsNumber(tsNode) && cJSON_IsString(nonceNode) &&
                    cJSON_IsString(reqNode) && cJSON_IsString(macNode);
    if (!ok) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Authenticated command missing required fields.");
        return true;
    }
    const uint64_t ts = static_cast<uint64_t>(tsNode->valuedouble);
    const std::string nonce = nonceNode->valuestring;
    const std::string reqId = reqNode->valuestring;
    const std::string mac = macNode->valuestring;

    const std::string expected = makeCommandMac(ts, nonce, reqId, action, key);
    if (expected.size() != mac.size() ||
        sodium_memcmp(expected.data(), mac.data(), mac.size()) != 0) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Authenticated command MAC mismatch; rejected.");
        if (m_audit) {
            m_audit->record(AuditManager::MQTT_UNLOCK_REQUEST, AuditManager::SOURCE_MQTT,
                            AuditManager::RESULT_FAILURE, m_node->info().node_id, "bad_mac");
        }
        return true;
    }

    // Freshness: optional wall-clock window; replay is stopped by the nonce set.
    const time_t now = time(nullptr);
    if (now > 1000000000) {
        const int64_t skew = static_cast<int64_t>(ts) - static_cast<int64_t>(now);
        if (skew < -300 || skew > 300) {
            cJSON_Delete(root);
            ESP_LOGW(TAG, "Authenticated command outside time window; rejected.");
            return true;
        }
    }

    // Replay protection.
    if (std::find(m_seenNonces.begin(), m_seenNonces.end(), nonce) != m_seenNonces.end()) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Replayed command nonce rejected.");
        return true;
    }
    m_seenNonces.push_back(nonce);
    if (m_seenNonces.size() > 32) {
        m_seenNonces.pop_front();
    }
    cJSON_Delete(root);

    EventLockState s{.source = LockManager::MQTT};
    s.currentState = LockManager::UNKNOWN;
    s.targetState = (action == "unlock") ? LockManager::UNLOCKED : LockManager::LOCKED;
    std::array<uint8_t, sizeof(EventLockState)> d{};
    const size_t dLen = alpaca::serialize(s, d);
    AppEventLoop::publish(LOCK_EVENT, LOCK_TARGET_STATE_CHANGED, d.data(), dLen);

    if (m_audit) {
        m_audit->record(action == "unlock" ? AuditManager::MQTT_UNLOCK_REQUEST : AuditManager::LOCK,
                        AuditManager::SOURCE_MQTT, AuditManager::RESULT_SUCCESS,
                        m_node->info().node_id, reqId);
    }
    ESP_LOGI(TAG, "Authenticated %s command accepted (req %s).", action.c_str(), reqId.c_str());
    return true;
}
