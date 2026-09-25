#include "DeviceCert.hpp"

#include "ConfigManager.hpp"
#include "config.hpp"

#include "esp_log.h"
#include "sodium.h"

#include "mbedtls/asn1.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

#include <array>
#include <cstring>
#include <vector>

namespace deviceCert {
namespace {

const char *TAG = "DeviceCert";

/// Long enough that a deployed device does not need re-provisioning, and a fixed
/// not-before so the certificate is valid even though a device straight out of the box
/// has no time source at all (no RTC, and NTP only becomes reachable after setup).
constexpr const char *kNotBefore = "20200101000000";
constexpr const char *kNotAfter = "20400101000000";

/// Write the certificate to PEM at most this big; an ECDSA P-256 cert is well under 1 kB.
constexpr size_t kPemBufferSize = 2048;

/// mbedTLS RNG callback backed by libsodium, which the firmware already links and
/// trusts for key material. Avoids pulling in and seeding a separate CTR_DRBG.
int sodiumRandom(void * /*context*/, unsigned char *output, size_t length) {
  randombytes_buf(output, length);
  return 0;
}

struct PkContext {
  mbedtls_pk_context ctx;
  PkContext() { mbedtls_pk_init(&ctx); }
  ~PkContext() { mbedtls_pk_free(&ctx); }
  PkContext(const PkContext &) = delete;
  PkContext &operator=(const PkContext &) = delete;
};

struct CrtWriter {
  mbedtls_x509write_cert ctx;
  CrtWriter() { mbedtls_x509write_crt_init(&ctx); }
  ~CrtWriter() { mbedtls_x509write_crt_free(&ctx); }
  CrtWriter(const CrtWriter &) = delete;
  CrtWriter &operator=(const CrtWriter &) = delete;
};

/// Build a fresh ECDSA P-256 key and a self-signed certificate for it.
bool generateSelfSigned(const std::string &commonName, std::string &certPem,
                        std::string &keyPem) {
  PkContext key;
  if (mbedtls_pk_setup(&key.ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
    ESP_LOGE(TAG, "Could not set up the key context");
    return false;
  }
  if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key.ctx), sodiumRandom,
                          nullptr) != 0) {
    ESP_LOGE(TAG, "Could not generate the ECDSA key");
    return false;
  }

  CrtWriter crt;
  mbedtls_x509write_crt_set_version(&crt.ctx, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt.ctx, MBEDTLS_MD_SHA256);

  const std::string subject = "CN=" + commonName;
  if (mbedtls_x509write_crt_set_subject_name(&crt.ctx, subject.c_str()) != 0 ||
      mbedtls_x509write_crt_set_issuer_name(&crt.ctx, subject.c_str()) != 0) {
    ESP_LOGE(TAG, "Could not set the certificate name");
    return false;
  }

  if (mbedtls_x509write_crt_set_validity(&crt.ctx, kNotBefore, kNotAfter) != 0) {
    ESP_LOGE(TAG, "Could not set the certificate validity");
    return false;
  }

  // 16 random bytes with the top bit cleared, so the serial is positive and unique per
  // device rather than the constant that most examples use.
  std::array<unsigned char, 16> serial{};
  randombytes_buf(serial.data(), serial.size());
  serial[0] &= 0x7F;
  if (mbedtls_x509write_crt_set_serial_raw(&crt.ctx, serial.data(), serial.size()) != 0) {
    ESP_LOGE(TAG, "Could not set the certificate serial");
    return false;
  }

  // A TLS server certificate: not a CA, usable for signatures, and explicitly for
  // server authentication. Clients that ignore the pin will still reject it for lack of
  // a trusted issuer, which is the intent.
  if (mbedtls_x509write_crt_set_basic_constraints(&crt.ctx, 0, -1) != 0) {
    ESP_LOGE(TAG, "Could not set basic constraints");
    return false;
  }
  if (mbedtls_x509write_crt_set_key_usage(&crt.ctx,
                                          MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                              MBEDTLS_X509_KU_KEY_AGREEMENT) != 0) {
    ESP_LOGE(TAG, "Could not set the key usage");
    return false;
  }
  mbedtls_asn1_sequence serverAuth{};
  serverAuth.buf.tag = MBEDTLS_ASN1_OID;
  serverAuth.buf.p = const_cast<unsigned char *>(
      reinterpret_cast<const unsigned char *>(MBEDTLS_OID_SERVER_AUTH));
  serverAuth.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
  serverAuth.next = nullptr;
  if (mbedtls_x509write_crt_set_ext_key_usage(&crt.ctx, &serverAuth) != 0) {
    ESP_LOGE(TAG, "Could not set the extended key usage");
    return false;
  }

  mbedtls_x509write_crt_set_subject_key(&crt.ctx, &key.ctx);
  mbedtls_x509write_crt_set_issuer_key(&crt.ctx, &key.ctx);

  std::vector<unsigned char> buffer(kPemBufferSize, 0);
  if (mbedtls_x509write_crt_pem(&crt.ctx, buffer.data(), buffer.size(), sodiumRandom,
                                nullptr) != 0) {
    ESP_LOGE(TAG, "Could not write the certificate as PEM");
    return false;
  }
  certPem.assign(reinterpret_cast<const char *>(buffer.data()));

  std::fill(buffer.begin(), buffer.end(), 0);
  if (mbedtls_pk_write_key_pem(&key.ctx, buffer.data(), buffer.size()) != 0) {
    ESP_LOGE(TAG, "Could not write the private key as PEM");
    return false;
  }
  keyPem.assign(reinterpret_cast<const char *>(buffer.data()));

  return true;
}

} // namespace

std::string certificateFingerprint(const std::string &pemCertificate) {
  if (pemCertificate.empty()) {
    return {};
  }

  mbedtls_x509_crt parsed;
  mbedtls_x509_crt_init(&parsed);
  const int rc = mbedtls_x509_crt_parse(
      &parsed, reinterpret_cast<const unsigned char *>(pemCertificate.c_str()),
      pemCertificate.size() + 1);
  if (rc != 0) {
    ESP_LOGW(TAG, "Could not parse the stored certificate (rc=%d)", rc);
    mbedtls_x509_crt_free(&parsed);
    return {};
  }

  // Hash the DER rather than the PEM: PEM line wrapping and the trailing newline are not
  // part of the certificate, so two encodings of the same certificate would otherwise
  // produce different fingerprints.
  std::array<unsigned char, 32> digest{};
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const int mdRc = (info == nullptr) ? -1
                                     : mbedtls_md(info, parsed.raw.p, parsed.raw.len,
                                                  digest.data());
  mbedtls_x509_crt_free(&parsed);
  if (mdRc != 0) {
    ESP_LOGW(TAG, "Could not hash the certificate");
    return {};
  }

  static const char *kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(digest.size() * 3);
  for (size_t i = 0; i < digest.size(); ++i) {
    if (i != 0) {
      out.push_back(':');
    }
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0F]);
  }
  return out;
}

std::string ensureSelfSignedCertificate(ConfigManager &configManager, bool *generatedOut) {
  if (generatedOut != nullptr) {
    *generatedOut = false;
  }
  const espConfig::https_certs_t &existing = configManager.getHttpsCertsConfig();

  if (!existing.serverCert.empty() && !existing.privateKey.empty()) {
    const std::string fingerprint = certificateFingerprint(existing.serverCert);
    if (!fingerprint.empty()) {
      ESP_LOGI(TAG, "Using the stored self-signed certificate (%s)", fingerprint.c_str());
      return fingerprint;
    }
    // A stored certificate that will not parse cannot be trusted or replaced
    // automatically without risking a device that is reachable today becoming
    // unreachable. Report it and leave the operator to clear it.
    ESP_LOGW(TAG, "The stored certificate could not be parsed; keeping it as-is");
    return {};
  }

  // Name the device after its own identity so a certificate is recognisable in a client
  // list, and so two devices cannot present the same subject.
  const espConfig::misc_config_t &misc = configManager.getConfig<espConfig::misc_config_t>();
  std::string commonName = misc.deviceName.empty() ? std::string(DEVICE_NAME) : misc.deviceName;

  std::string certPem;
  std::string keyPem;
  ESP_LOGW(TAG, "No TLS certificate on this device; generating a self-signed one");
  if (!generateSelfSigned(commonName, certPem, keyPem)) {
    return {};
  }

  if (!configManager.saveCertificate(espConfig::CertType::HTTPS_SERVER_CERT, certPem) ||
      !configManager.saveCertificate(espConfig::CertType::HTTPS_PRIVATE_KEY, keyPem)) {
    ESP_LOGE(TAG, "Could not store the generated certificate");
    return {};
  }

  const std::string fingerprint = certificateFingerprint(certPem);
  if (generatedOut != nullptr) {
    *generatedOut = true;
  }
  ESP_LOGW(TAG, "Generated a self-signed certificate for CN=%s", commonName.c_str());
  ESP_LOGW(TAG, "  SHA-256 fingerprint: %s",
           fingerprint.empty() ? "(unavailable)" : fingerprint.c_str());
  ESP_LOGW(TAG, "Clients should pin this fingerprint; it changes if the device is reset.");
  return fingerprint;
}

} // namespace deviceCert
