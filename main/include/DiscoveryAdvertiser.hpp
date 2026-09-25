#pragma once

#include <cstdint>
#include <string>

class ConfigManager;
class NodeIdentityManager;
class WebServerManager;

/**
 * Advertises the device REST API over mDNS so Home Assistant can find it without a
 * broker and without the user typing an IP address.
 *
 * This deliberately does not create its own responder. HomeSpan already brings up
 * espressif__mdns for HAP (Arduino's ESPmDNS is a thin wrapper over the very same
 * component), so this adds one more service to that stack - a second responder on one
 * device would fight over the socket.
 */
class DiscoveryAdvertiser {
public:
  void begin(ConfigManager &configManager, NodeIdentityManager &nodeIdentityManager,
             WebServerManager &webServerManager);

  /**
   * @brief Re-advertise if the record has gone missing.
   *
   * HomeSpan stops mDNS on its own reset paths (MDNS.end()), which silently takes this
   * service down with it. Cheap to call and internally rate limited, so the main loop
   * can call it unconditionally.
   */
  void refresh();

  /**
   * @brief Publish immediately, skipping the retry throttle.
   *
   * Called once the station interface is up and the web server has a port - until then
   * there is nothing worth pointing a client at.
   */
  void onNetworkUp();

  void stop();

private:
  bool advertise();
  static bool isStationMode();

  ConfigManager *m_configManager = nullptr;
  NodeIdentityManager *m_nodeIdentityManager = nullptr;
  WebServerManager *m_webServerManager = nullptr;

  /// Computed once during begin() so the PEM is not re-parsed on every re-advertise.
  std::string m_fingerprint;
  std::string m_instanceName;

  bool m_advertised = false;
  int64_t m_lastAttemptUs = 0;
};
