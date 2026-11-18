#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace lume::platform::windows::quota {

// The DeepSeek API key, encrypted with the current user's DPAPI so no other
// account on the machine can read it and no plaintext copy reaches the disk.
// The key never leaves this module except to the request that uses it.
class DeepSeekKeyStore final {
public:
    // `path` is the encrypted blob's file; the parent must exist.
    explicit DeepSeekKeyStore(std::filesystem::path path);
    ~DeepSeekKeyStore();
    DeepSeekKeyStore(const DeepSeekKeyStore&) = delete;
    DeepSeekKeyStore& operator=(const DeepSeekKeyStore&) = delete;

    // Reads the saved key, or nullopt when none is stored or it cannot be
    // decrypted. An unreadable blob is not an error the caller must handle:
    // the user can simply save the key again.
    std::optional<std::string> Load() const;
    // Saves `key`; false reports a write or DPAPI failure. An empty key clears.
    bool Save(std::string key) const;
    void Clear() const;
    // True when a blob exists. Does not decrypt it.
    bool Exists() const;

private:
    std::filesystem::path path_;
};

}  // namespace lume::platform::windows::quota
