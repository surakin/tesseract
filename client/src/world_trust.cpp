#include <tesseract/paths.h>
#include <tesseract/world_trust.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace tesseract
{

namespace
{
long process_id()
{
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

bool canonical_hash(const std::string& h)
{
    if (h.size() != 64)
    {
        return false;
    }
    for (char c : h)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}
} // namespace

WorldTrustStore::WorldTrustStore(std::filesystem::path file) : file_(std::move(file))
{
}

std::filesystem::path WorldTrustStore::default_path()
{
    return data_dir() / "trusted_world_bundles.json";
}

std::set<std::string> WorldTrustStore::read_file_() const
{
    constexpr std::uintmax_t kMaxBytes = 1024 * 1024;
    std::set<std::string> out;
    try
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file_, ec);
        if (ec || size > kMaxBytes)
        {
            return out;
        }
        std::ifstream in(file_, std::ios::binary);
        if (!in)
        {
            return out;
        }
        auto j = nlohmann::json::parse(in);
        if (!j.is_object() || !j.contains("trusted") || !j["trusted"].is_array())
        {
            return out;
        }
        for (const auto& v : j["trusted"])
        {
            if (v.is_string() && canonical_hash(v.get<std::string>()))
            {
                out.insert(v.get<std::string>());
            }
        }
    }
    catch (const std::exception&)
    {
        out.clear();
    }
    return out;
}

bool WorldTrustStore::trusted(const std::string& hash) const
{
    if (!canonical_hash(hash))
    {
        return false;
    }
    std::lock_guard lk(mu_);
    return read_file_().count(hash) != 0;
}

bool WorldTrustStore::add(const std::string& hash)
{
    constexpr std::size_t kMaxEntries = 10000;
    if (!canonical_hash(hash))
    {
        return false;
    }
    std::lock_guard lk(mu_);
    auto hashes = read_file_();
    if (hashes.count(hash))
    {
        return true;
    }
    if (hashes.size() >= kMaxEntries)
    {
        return false;
    }
    hashes.insert(hash);
    nlohmann::json j;
    j["version"] = 1;
    j["trusted"] = nlohmann::json::array();
    for (const auto& h : hashes)
    {
        j["trusted"].push_back(h);
    }

    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    if (file_.has_parent_path())
    {
        std::filesystem::create_directories(file_.parent_path(), ec);
    }
    auto tmp = file_;
    tmp += "." + std::to_string(process_id()) + "." + std::to_string(counter++) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return false;
        }
        out << j.dump(2);
        out.flush();
        if (!out)
        {
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec)
    {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

WorldConsent decide_consent(bool scripts_are_builtin, const std::string& hash, bool sender_verified,
                            const WorldTrustStore& store)
{
    if (scripts_are_builtin || store.trusted(hash) || sender_verified)
    {
        return WorldConsent::Run;
    }
    return WorldConsent::Ask;
}

bool user_trust_is_verified(UserTrust trust)
{
    return trust == UserTrust::Verified;
}

} // namespace tesseract
