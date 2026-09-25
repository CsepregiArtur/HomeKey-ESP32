#pragma once
#include "ddk/store/CredentialStore.h"
#include "ddk/store/ReaderIdentity.h"
#include "ddk/store/Issuer.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <nvs.h>
#include <string>
#include <vector>

/**
 * @class NvsCredentialStore
 * @brief NVS-backed ddk::CredentialStore.
 *
**/
class NvsCredentialStore : public ddk::CredentialStore {
public:
    NvsCredentialStore();
    ~NvsCredentialStore() override;

    /** Opens the "SAVED_DATA" namespace and loads existing data. */
    bool begin();

    const ddk::ReaderIdentity& reader_identity() const override;
    void provision_identity(const ddk::ReaderIdentity&) override;
    ddk::span<ddk::Issuer> issuers() override;
    void save() override;

    struct Snapshot {
        ddk::ReaderIdentity identity;
        std::vector<ddk::Issuer> issuers;
        /// Issuer id (uppercase hex) -> the name the user gave it. Absent when unnamed.
        std::map<std::string, std::string> issuer_labels;
    };
    /** Consistent copy of everything, for UI/telemetry tasks. */
    Snapshot snapshot() const;

    /**
     * @brief Longest label accepted.
     *
     * Bounded so a pasted essay cannot grow the NVS blob indefinitely: the blob is
     * rewritten in full on every save, not appended to.
     */
    static constexpr size_t kMaxIssuerLabelLength = 64;

    /**
     * @brief Name an issuer, or clear its name by passing an empty label.
     *
     * The label is the user's own words for a HomeKit admin controller. It is not
     * cryptographic identity and never leaves the device as an identifier: see
     * ``issuerLabel`` and the household contract for what is published.
     *
     * Returns false without changing anything when the label is too long. Does NOT
     * save - the caller calls save(), matching the other mutators.
     */
    bool setIssuerLabel(const std::vector<uint8_t>& issuerId, const std::string& label);

    /**
     * @brief The name the user gave an issuer, or an empty string when it has none.
     *
     * An unnamed issuer is named *nothing*, never a generated placeholder: a caller
     * that shows something instead would be inventing an identification the user did
     * not make.
     */
    std::string issuerLabel(const std::vector<uint8_t>& issuerId) const;

    /** Clears reader key material. */
    bool eraseReaderKey();

    /** Erases everything and publishes ACCESSDATA_CHANGED. */
    bool deleteAllReaderData();

    /** Adds an issuer if absent. Does NOT save — caller calls save(). */
    bool addIssuerIfNotExists(const std::vector<uint8_t>& issuerId,
                              const uint8_t* publicKey);

    /** Removes an issuer if present. Does NOT save — caller calls save(). */
    bool removeIssuerIfExists(const std::vector<uint8_t>& issuerId);

private:
    void load();

    /// Uppercase hex of an issuer id, as the key labels are stored under.
    static std::string labelKey(const std::vector<uint8_t>& issuerId);

    ddk::ReaderIdentity identity_;
    std::vector<ddk::Issuer> issuers_;
    std::map<std::string, std::string> issuer_labels_;
    mutable std::mutex mutex_;
    nvs_handle handle_{};
    bool initialized_ = false;

    static const char* TAG;
    static const char* NVS_KEY;
};
