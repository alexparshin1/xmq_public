#include "server/Settings/PasswordHash.h"
#include "server/Settings/PasswordImport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace std;
using namespace sptk;
using namespace xmq;
namespace ranges = std::ranges;

namespace {

#ifndef XMQ_TEST_MOSQUITTO_IMPORT_DIRECTORY
#error "XMQ_TEST_MOSQUITTO_IMPORT_DIRECTORY is not defined"
#endif

const filesystem::path corpusDirectory {XMQ_TEST_MOSQUITTO_IMPORT_DIRECTORY};

string readFile(const string& name)
{
    ifstream stream(corpusDirectory / name);
    if (!stream)
    {
        throw runtime_error("Cannot read " + name);
    }
    stringstream content;
    content << stream.rdbuf();
    return content.str();
}

const PasswordImport::Entry* find(const vector<PasswordImport::Entry>& entries, const string& username)
{
    const auto found = ranges::find_if(entries, [&](const auto& entry)
                                      { return entry.m_username == username; });
    return found == entries.end() ? nullptr : &*found;
}

} // namespace

TEST(PasswordImport, readsAMosquittoPbkdf2VerifierAsItIs)
{
    const auto entries = PasswordImport::parse(readFile("password-file-pbkdf2.pwfile"));
    ASSERT_EQ(1U, entries.size());
    EXPECT_EQ("user", entries.front().m_username);
    EXPECT_EQ(PasswordImport::Status::New, entries.front().m_status);
    EXPECT_FALSE(entries.front().m_plaintext);
    EXPECT_TRUE(PasswordHash::verify("password", entries.front().m_verifier));
}

TEST(PasswordImport, reportsAFormatItCannotCheckInsteadOfStoringIt)
{
    const auto entries = PasswordImport::parse(readFile("password-file-sha512-crypt.pwfile"));
    ASSERT_EQ(1U, entries.size());
    EXPECT_EQ(PasswordImport::Status::Unsupported, entries.front().m_status);
    EXPECT_NE(string::npos, entries.front().m_reason.find("SHA512-crypt"));
}

TEST(PasswordImport, aClearTextLineIsHashedLaterNotStoredAsItArrived)
{
    const auto entries = PasswordImport::parse(readFile("password-file-plain.pwfile"));
    ASSERT_EQ(1U, entries.size());
    EXPECT_EQ("user", entries.front().m_username);
    EXPECT_TRUE(entries.front().m_plaintext);
    EXPECT_EQ("password", entries.front().m_verifier);
    EXPECT_EQ(PasswordImport::Status::New, entries.front().m_status);
}

TEST(PasswordImport, theMixedFileSeparatesEveryKind)
{
    const auto entries = PasswordImport::parse(readFile("password-file-mixed.pwfile"));
    const auto counts = [&](const PasswordImport::Status status)
    {
        return ranges::count_if(entries, [&](const auto& entry) { return entry.m_status == status; });
    };

    // A byte-order mark and CRLF are not part of the first username.
    EXPECT_NE(nullptr, find(entries, "user"));
    EXPECT_EQ(2, counts(PasswordImport::Status::New));
    EXPECT_EQ(2, counts(PasswordImport::Status::Unsupported));
    EXPECT_EQ(2, counts(PasswordImport::Status::Invalid));

    const auto plain = find(entries, "plain");
    ASSERT_NE(nullptr, plain);
    EXPECT_TRUE(plain->m_plaintext);
    EXPECT_EQ("LetMeIn123!", plain->m_verifier);

    const auto argon = find(entries, "new");
    ASSERT_NE(nullptr, argon);
    EXPECT_NE(string::npos, argon->m_reason.find("Argon2id"));
}

TEST(PasswordImport, existingAccountsAreCountedAndNotChanged)
{
    const auto entries = PasswordImport::parse(
        "alice:$7$1000$h0tqVxBwkB9rKAXukTtffzdbBQtNy1q5FBTDwSW4hucfjpqunBbxW10NVnRk7Cfh0lQndnOv2+k4wJavgz1JNw==$02ujkUXlKkJGFzlQHNjUgXwG3XRB1mr3vs8NX5teGCJGbN4hdgSpHNHuj47j8r5SHXsO7GeHpmkpPNhLraVVcQ==\n"
        "bob:secret\n");
    const auto plan = PasswordImport::plan(entries, {"alice"});
    EXPECT_EQ(1U, plan.m_new.size());
    EXPECT_EQ(1U, plan.m_existing.size());
    EXPECT_EQ("bob", plan.m_new.front().m_username);
    EXPECT_EQ(PasswordImport::Status::Existing, plan.m_existing.front().m_status);
}

TEST(PasswordImport, thePlanNeverCarriesAVerifierOrAPassword)
{
    const auto entries = PasswordImport::parse(readFile("password-file-mixed.pwfile"));
    const auto plan = PasswordImport::plan(entries, {});
    const auto json = PasswordImport::toJson(plan);

    EXPECT_NE(string::npos, json.find("\"plain\""));
    EXPECT_EQ(string::npos, json.find("$7$")) << "a verifier leaked into the plan";
    EXPECT_EQ(string::npos, json.find("LetMeIn123!")) << "a clear password leaked into the plan";
}
