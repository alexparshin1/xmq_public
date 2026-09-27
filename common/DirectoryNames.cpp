/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "DirectoryNames.h"

#include "base/xmq-config.h"

#include <filesystem>

#ifdef _WIN32
// WIN32_LEAN_AND_MEAN keeps windows.h from pulling in winsock.h (v1), which would clash with the
// winsock2.h that SPTK headers bring in. Nothing here needs the sockets declarations.
#define WIN32_LEAN_AND_MEAN
#include <vector>
#include <windows.h>
#endif

using namespace std;
using namespace xmq;

std::filesystem::path DirectoryNames::binDirectory()
{
#ifdef _WIN32
    return "C:/Program Files/xmq";
#else
    return "/usr/local/bin";
#endif
}

std::filesystem::path DirectoryNames::dataDirectory()
{
#ifdef _WIN32
    return "C:/ProgramData/xmq";
#else
    return "/var/lib/xmq";
#endif
}

#ifdef _WIN32
namespace {
/**
 * @brief The directory the running executable is in, or empty when it cannot be determined.
 */
std::filesystem::path executableDirectory()
{
    // The wide variant, because filesystem::path is natively wchar_t on Windows: the ANSI one
    // would round-trip through the active code page and mangle any non-ASCII directory name.
    //
    // GetModuleFileNameW returns the character count excluding the terminator, or exactly the
    // buffer size when the name did not fit, so the buffer grows until a call comes back short.
    // MAX_PATH is only a starting guess - it stopped being a ceiling once long paths were enabled.
    constexpr size_t maxWindowsPath = 32768;

    for (vector<wchar_t> buffer(MAX_PATH);;)
    {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || buffer.size() >= maxWindowsPath)
        {
            return {}; // Failed outright, or grew past any legal path length.
        }
        if (length < buffer.size())
        {
            return std::filesystem::path(buffer.data()).parent_path(); // Null-terminated on success.
        }
        buffer.resize(buffer.size() * 2);
    }
}
} // namespace
#endif

namespace {
/// Where the web interface files come from instead of the installed directory, when a test has
/// said so.
std::filesystem::path webfaceDirectoryOverride;
} // namespace

void DirectoryNames::setWebfaceDirectory(const std::filesystem::path& directory)
{
    webfaceDirectoryOverride = directory;
}

std::filesystem::path DirectoryNames::webfaceDirectory()
{
    if (!webfaceDirectoryOverride.empty())
    {
        return webfaceDirectoryOverride;
    }

#ifdef _WIN32
    // The installation directory is chosen when the installer runs, so a path fixed at build time
    // would only be right for an installation left in its default place. The layout around the
    // executable is what the installer guarantees: <install directory>/bin/xmq_server.exe next to
    // <install directory>/react.
    if (const auto binaries = executableDirectory(); !binaries.empty())
    {
        return binaries.parent_path() / "react";
    }
#endif

    // The default installation directory, which is all the build knows.
    return REACT_DIRECTORY;
}

std::filesystem::path DirectoryNames::confDirectory()
{
    // Where this broker was built to keep its configuration. A packaged one answers /etc/xmq; one
    // installed under somebody's home answers a directory there, because nothing in such an
    // installation may need a privilege to write. The choice belongs to the build, which knows the
    // prefix, and not to an #ifdef here, which only knows the platform.
    return XMQ_CONF_DIRECTORY;
}

std::filesystem::path DirectoryNames::tempDirectory()
{
#ifdef _WIN32
    return "C:/Windows/temp";
#else
    return "/tmp";
#endif
}

std::filesystem::path DirectoryNames::logsDirectory()
{
    return XMQ_LOGS_DIRECTORY;
}

namespace {
/// Where certificates go instead of the installed directory, when something has said so.
std::filesystem::path certsDirectoryOverride;
} // namespace

std::filesystem::path DirectoryNames::certsDirectory()
{
    if (!certsDirectoryOverride.empty())
    {
        return certsDirectoryOverride;
    }

    return XMQ_CERTS_DIRECTORY;
}

void DirectoryNames::setCertsDirectory(const std::filesystem::path& directory)
{
    certsDirectoryOverride = directory;
}

sptk::String DirectoryNames::sqliteUri(const std::filesystem::path& file)
{
    // The native spelling, not generic_string(): on Windows SPTK hands the whole thing to
    // sqlite3_open, which wants backslashes, and the path is not split on them anyway. On Unix
    // the two are the same.
    const auto path = file.string();

    // The slash the URI needs in order to have a path at all. An absolute Unix path brings its
    // own, so the URI ends up with two, which is correct and is what the parser expects: the
    // first delimits, the second belongs to the file name.
    return sptk::String("sqlite3://localhost/" + path);
}
