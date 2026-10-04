#pragma once

#include <string>

class ConfigManager;

/**
 * On-device self-signed TLS identity.
 *
 * The device needs a certificate before it can serve HTTPS, and there is no way to ship
 * one that is meaningful: a certificate baked into the firmware would be identical on
 * every unit, so pinning it would prove nothing. Generating the key and certificate on
 * the device, once, gives each unit a distinct identity that can be pinned by a client
 * the first time it connects.
 *
 * The certificate is self-signed, so it carries no chain to a public CA - the value of
 * the pin is the key being unique per device, not the issuer. Clients are expected to
 * remember the fingerprint reported here and refuse to talk to a device whose
 * certificate has changed.
 */
namespace deviceCert {

/**
 * @brief Make sure this device has a certificate and key, generating them if needed.
 *
 * Reuses anything already stored, so this is safe to call on every boot. On success the
 * certificate and key are persisted through ConfigManager, which is what the HTTPS
 * server reads.
 *
 * @param configManager used to read and store the certificate pair.
 * @param generatedOut optional; set to true only when a new certificate was created on
 *        this call. Callers use it to distinguish "this device has an identity" from
 *        "this device just acquired its first one", which is a one-time event.
 * @param address optional; the IPv4 address the device is reached on. When given, the
 *        certificate is issued with that address as a subjectAltName, and a stored
 *        certificate that does not already cover it is replaced. Without a matching
 *        subjectAltName a browser reports a name mismatch even when the certificate
 *        itself has been trusted, so a certificate that cannot name its own address
 *        cannot produce a warning-free page. Callers that do not know the address yet
 *        (the boot-time call does not, because a station address only exists once the
 *        association succeeds) pass nothing and keep whatever is stored.
 * @return the SHA-256 fingerprint of the certificate as colon-separated uppercase hex,
 *         or an empty string if no identity could be established.
 */
std::string ensureSelfSignedCertificate(ConfigManager &configManager,
                                       bool *generatedOut = nullptr,
                                       const std::string &address = {});

/**
 * @brief SHA-256 fingerprint of a PEM certificate, colon-separated uppercase hex.
 *
 * Returns an empty string if the PEM cannot be parsed.
 */
std::string certificateFingerprint(const std::string &pemCertificate);

} // namespace deviceCert
