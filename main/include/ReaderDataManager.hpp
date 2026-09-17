#pragma once
#include "ddk/store/CredentialStore.h"
#include "ddk/store/ReaderIdentity.h"
#include "ddk/store/Issuer.h"
#include <mutex>
#include <nvs.h>
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
    };
    /** Consistent copy of everything, for UI/telemetry tasks. */
    Snapshot snapshot() const;

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

    ddk::ReaderIdentity identity_;
    std::vector<ddk::Issuer> issuers_;
    mutable std::mutex mutex_;
    nvs_handle handle_{};
    bool initialized_ = false;

    static const char* TAG;
    static const char* NVS_KEY;
};
