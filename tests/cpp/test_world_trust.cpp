// Trusted world-bundle hashes and the consent decision. Files live under the
// isolation listener's temporary data dir, never the real one.

#include <catch2/catch_test_macros.hpp>

#include <tesseract/paths.h>
#include <tesseract/world_trust.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <cstdio>
#include <fstream>

using namespace tesseract;

namespace
{
const std::string kA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const std::string kB = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

std::filesystem::path store_file()
{
    auto p = data_dir() / "trust_test" / "trusted.json";
    std::filesystem::create_directories(p.parent_path());
    return p;
}
} // namespace

TEST_CASE("trust store persists across instances", "[world_trust]")
{
    auto f = store_file();
    {
        WorldTrustStore s(f);
        CHECK_FALSE(s.trusted(kA));
        s.add(kA);
        CHECK(s.trusted(kA));
    }
    WorldTrustStore again(f);
    CHECK(again.trusted(kA));
    CHECK_FALSE(again.trusted(kB));
}

TEST_CASE("corrupt file is empty not trusted", "[world_trust]")
{
    auto f = store_file();
    {
        std::ofstream(f) << "{ not json";
    }
    WorldTrustStore s(f);
    CHECK_FALSE(s.trusted(kA));
    s.add(kB);
    WorldTrustStore again(f);
    CHECK(again.trusted(kB));
    CHECK_FALSE(again.trusted(kA));
}

TEST_CASE("missing file is empty", "[world_trust]")
{
    WorldTrustStore s(data_dir() / "nope" / "missing.json");
    CHECK_FALSE(s.trusted(kA));
}

TEST_CASE("add is idempotent", "[world_trust]")
{
    auto f = store_file();
    WorldTrustStore s(f);
    s.add(kA);
    s.add(kA);
    WorldTrustStore again(f);
    CHECK(again.trusted(kA));
    std::ifstream in(f);
    auto j = nlohmann::json::parse(in);
    CHECK(j["trusted"].size() == 1);
    for (const auto& e : std::filesystem::directory_iterator(f.parent_path()))
    {
        CHECK(e.path() == f);
    }
}

TEST_CASE("unknown hash never trusted", "[world_trust]")
{
    WorldTrustStore s(store_file());
    s.add(kA);
    CHECK_FALSE(s.trusted(kB));
    CHECK_FALSE(s.trusted(""));
    // Non-canonical forms are untrusted and never stored.
    std::string upper = kA;
    for (auto& c : upper)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    CHECK_FALSE(s.trusted(upper));
    s.add(upper);
    s.add("short");
    CHECK_FALSE(s.trusted(upper));
    CHECK_FALSE(s.trusted("short"));
}

TEST_CASE("decide_consent order", "[world_trust][consent]")
{
    WorldTrustStore s(store_file());
    s.add(kA);

    // builtin -> Run even when untrusted and unverified
    CHECK(decide_consent(true, kB, false, s) == WorldConsent::Run);
    // trusted hash from an unverified sender
    CHECK(decide_consent(false, kA, false, s) == WorldConsent::Run);
    // verified sender, hash never seen (changed bundle)
    CHECK(decide_consent(false, kB, true, s) == WorldConsent::Run);
    // none
    CHECK(decide_consent(false, kB, false, s) == WorldConsent::Ask);
    // builtin with an empty hash
    CHECK(decide_consent(true, "", false, s) == WorldConsent::Run);
    // verified AND trusted
    CHECK(decide_consent(false, kA, true, s) == WorldConsent::Run);
    // nothing, empty hash
    CHECK(decide_consent(false, "", false, s) == WorldConsent::Ask);
}

TEST_CASE("consent counts only Verified senders", "[world_trust][consent]")
{
    CHECK(user_trust_is_verified(UserTrust::Verified));
    CHECK_FALSE(user_trust_is_verified(UserTrust::Unknown));
    CHECK_FALSE(user_trust_is_verified(UserTrust::NotVerified));
    CHECK_FALSE(user_trust_is_verified(UserTrust::VerificationViolation));
}

TEST_CASE("two instances on one file do not lose updates", "[world_trust]")
{
    auto f = store_file();
    WorldTrustStore a(f), b(f);
    CHECK_FALSE(a.trusted(kB));
    CHECK(a.add(kA));
    CHECK(b.add(kB));
    CHECK(a.trusted(kA));
    CHECK(a.trusted(kB));
    CHECK(b.trusted(kA));
    CHECK(b.trusted(kB));
}

TEST_CASE("failed persistence is reported and not trusted", "[world_trust]")
{
    auto blocker = data_dir() / "trust_blocker";
    std::filesystem::create_directories(blocker.parent_path());
    {
        std::ofstream(blocker) << "x";
    }
    WorldTrustStore s(blocker / "trusted.json");
    CHECK_FALSE(s.add(kA));
    CHECK_FALSE(s.trusted(kA));
}

TEST_CASE("wrong-shaped trust file is empty and rewritten", "[world_trust]")
{
    for (const char* body : {"[]", "null", R"({"trusted":[1,2]})", R"({"trusted":"x"})"})
    {
        auto f = store_file();
        {
            std::ofstream(f) << body;
        }
        WorldTrustStore s(f);
        CHECK_FALSE(s.trusted(kA));
        CHECK(s.add(kA));
        WorldTrustStore again(f);
        CHECK(again.trusted(kA));
        std::ifstream in(f);
        CHECK(nlohmann::json::parse(in)["trusted"].size() == 1);
    }
}

TEST_CASE("oversized trust file is empty", "[world_trust]")
{
    auto f = store_file();
    {
        std::ofstream out(f);
        out << R"({"trusted":[")" << kA << R"("],"pad":")" << std::string(1100 * 1024, 'x')
            << R"("})";
    }
    WorldTrustStore s(f);
    CHECK_FALSE(s.trusted(kA));
}

TEST_CASE("full trust list refuses add", "[world_trust]")
{
    auto f = store_file();
    nlohmann::json j;
    j["trusted"] = nlohmann::json::array();
    for (int i = 0; i < 10000; ++i)
    {
        char buf[65];
        std::snprintf(buf, sizeof buf, "%064x", i);
        j["trusted"].push_back(buf);
    }
    {
        std::ofstream(f) << j.dump();
    }
    WorldTrustStore s(f);
    CHECK_FALSE(s.add(kA));
    CHECK_FALSE(s.trusted(kA));
    CHECK(s.add(std::string(64, '0')));
}
