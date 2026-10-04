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
#include <cstdio>
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

/// Encode a dotted-quad address into the four bytes an iPAddress subjectAltName carries.
/// Returns false for anything that is not a plain IPv4 literal, so a malformed value is
/// never written into a certificate.
bool ipv4ToBytes(const std::string &text, std::array<unsigned char, 4> &out) {
  unsigned a = 0;
  unsigned b = 0;
  unsigned c = 0;
  unsigned d = 0;
  char tail = 0;
  if (sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
    return false;
  }
  if (a > 255 || b > 255 || c > 255 || d > 255) {
    return false;
  }
  out = {static_cast<unsigned char>(a), static_cast<unsigned char>(b),
         static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
  return true;
}

/// Build a fresh ECDSA P-256 key and a self-signed certificate for it.
bool generateSelfSigned(const std::string &commonName, const std::string &address,
                        std::string &certPem, std::string &keyPem) {
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

  // The subjectAltName is what a browser actually matches against the address in the URL.
  // It has been required since 2017, when the common name stopped being considered for that
  // check, so a certificate without one is refused for every address - including by a client
  // that has deliberately chosen to trust this certificate. Writing it is therefore not
  // optional; it is what makes the self-signed identity usable at all.
  //
  // This mbedTLS has no setter for a certificate's subjectAltName (only a parser for reading
  // one back), so the extension value is encoded here. For a single iPAddress general name
  // that is short and fixed: SEQUENCE { [7] OCTET STRING(4) } is 30 06 87 04 followed by the
  // four address bytes.
  std::array<unsigned char, 4> ipBytes{};
  if (ipv4ToBytes(address, ipBytes)) {
    const unsigned char san[] = {0x30, 0x06, 0x87, 0x04, ipBytes[0], ipBytes[1], ipBytes[2],
                                 ipBytes[3]};
    if (mbedtls_x509write_crt_set_extension(
            &crt.ctx, MBEDTLS_OID_SUBJECT_ALT_NAME,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san,
            sizeof(san)) != 0) {
      ESP_LOGE(TAG, "Could not set the subject alternative name");
      return false;
    }
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

/// Does this PEM certificate already carry `address` as an iPAddress subjectAltName?
///
/// Looked for in the DER rather than through mbedTLS's parsed subjectAltNames, which hands
/// the names back as opaque blocks that still have to be walked to find out which one is
/// which: an iPAddress general name is written as the tag 0x87, a length of 4, then the four
/// address bytes, so there is a single unambiguous byte sequence to search for.
bool certificateCoversAddress(const std::string &pemCertificate, const std::string &address) {
  std::array<unsigned char, 4> ip{};
  if (pemCertificate.empty() || !ipv4ToBytes(address, ip)) {
    return false;
  }

  mbedtls_x509_crt parsed;
  mbedtls_x509_crt_init(&parsed);
  bool covered = false;
  if (mbedtls_x509_crt_parse(
          &parsed, reinterpret_cast<const unsigned char *>(pemCertificate.c_str()),
          pemCertificate.size() + 1) == 0) {
    const unsigned char *der = parsed.raw.p;
    const size_t derLen = parsed.raw.len;
    for (size_t i = 0; der != nullptr && i + 6 <= derLen; i++) {
      if (der[i] == 0x87 && der[i + 1] == 0x04 && der[i + 2] == ip[0] &&
          der[i + 3] == ip[1] && der[i + 4] == ip[2] && der[i + 5] == ip[3]) {
        covered = true;
        break;
      }
    }
  }
  mbedtls_x509_crt_free(&parsed);
  return covered;
}

/// Is this PEM certificate its own issuer, meaning this device generated it?
///
/// This decides whether the certificate may be replaced. One this device generated can be
/// re-issued freely, because nothing outside the device has any reason to know it. One that
/// came from a CA was installed deliberately through the Web UI, and re-issuing it because
/// the device's address changed would quietly withdraw the identity the operator's clients
/// were configured to trust, so that decision is left to a human.
bool certificateIsSelfIssued(const std::string &pemCertificate) {
  if (pemCertificate.empty()) {
    return false;
  }

  mbedtls_x509_crt parsed;
  mbedtls_x509_crt_init(&parsed);
  bool selfIssued = false;
  if (mbedtls_x509_crt_parse(
          &parsed, reinterpret_cast<const unsigned char *>(pemCertificate.c_str()),
          pemCertificate.size() + 1) == 0) {
    selfIssued = parsed.subject_raw.p != nullptr && parsed.issuer_raw.p != nullptr &&
                 parsed.subject_raw.len == parsed.issuer_raw.len &&
                 memcmp(parsed.subject_raw.p, parsed.issuer_raw.p,
                        parsed.subject_raw.len) == 0;
  }
  mbedtls_x509_crt_free(&parsed);
  return selfIssued;
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

std::string ensureSelfSignedCertificate(ConfigManager &configManager, bool *generatedOut,
                                        const std::string &address) {
  if (generatedOut != nullptr) {
    *generatedOut = false;
  }
  const espConfig::https_certs_t &existing = configManager.getHttpsCertsConfig();

  // An address that is not a plain IPv4 literal is not something a certificate can name, so
  // it is ignored rather than treated as an unmet requirement - otherwise an unparseable
  // value would make every boot replace the certificate and change the fingerprint that
  // clients were told to pin.
  std::array<unsigned char, 4> addressBytes{};
  const bool addressKnown = ipv4ToBytes(address, addressBytes);

  // A certificate is only replaced if this device issued it. See certificateIsSelfIssued().
  const bool replaceable = certificateIsSelfIssued(existing.serverCert);
  const bool coversAddress = certificateCoversAddress(existing.serverCert, address);

  if (!existing.serverCert.empty() && !existing.privateKey.empty() &&
      (!addressKnown || !replaceable || coversAddress)) {
    const std::string fingerprint = certificateFingerprint(existing.serverCert);
    if (!fingerprint.empty()) {
      if (addressKnown && !coversAddress) {
        // Kept on purpose, and worth saying out loud: the address in the URL will not match
        // this certificate, so the page will be refused until either the certificate is
        // replaced through the Web UI or the device keeps a stable address.
        ESP_LOGW(TAG, "The stored certificate was issued by a CA and does not name %s, so it "
                      "is being kept as installed; HTTPS to that address will be rejected "
                      "until it is replaced or the address stops changing.",
                 address.c_str());
      } else {
        ESP_LOGI(TAG, "Using the stored certificate (%s)", fingerprint.c_str());
      }
      return fingerprint;
    }
    // A stored certificate that will not parse cannot be trusted or replaced
    // automatically without risking a device that is reachable today becoming
    // unreachable. Report it and leave the operator to clear it.
    ESP_LOGW(TAG, "The stored certificate could not be parsed; keeping it as-is");
    return {};
  }

  if (!existing.serverCert.empty() && addressKnown && replaceable) {
    // Replacing a certificate changes the fingerprint clients were told to pin, so say so
    // rather than doing it silently. The old certificate is superseded because it cannot
    // name the address the device is now reached on, which no client would accept for that
    // address anyway.
    ESP_LOGW(TAG, "The stored certificate does not name %s; issuing a replacement with it "
                  "as a subjectAltName. The previous fingerprint is no longer valid.",
             address.c_str());
  }

  // Name the device after its own identity so a certificate is recognisable in a client
  // list, and so two devices cannot present the same subject.
  const espConfig::misc_config_t &misc = configManager.getConfig<espConfig::misc_config_t>();
  std::string commonName = misc.deviceName.empty() ? std::string(DEVICE_NAME) : misc.deviceName;

  std::string certPem;
  std::string keyPem;
  ESP_LOGW(TAG, "No TLS certificate on this device; generating a self-signed one");
  if (!generateSelfSigned(commonName, address, certPem, keyPem)) {
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
  if (addressKnown) {
    ESP_LOGW(TAG, "  subjectAltName: IP:%s", address.c_str());
  }
  ESP_LOGW(TAG, "  SHA-256 fingerprint: %s",
           fingerprint.empty() ? "(unavailable)" : fingerprint.c_str());
  ESP_LOGW(TAG, "Clients should pin this fingerprint; it changes if the device is reset.");
  return fingerprint;
}

} // namespace deviceCert
