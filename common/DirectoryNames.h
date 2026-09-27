/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#pragma once

#include "base/xmq.h"

#include <sptk5/String.h>

#include <filesystem>
#include <shared_mutex>
#include <string>

namespace xmq {

/**
 * @brief Important directory names.
 */
class XMQ_EXPORT DirectoryNames
{
public:
    /**
     * @brief The directory where executable files are stored.
     */
    [[nodiscard]] static std::filesystem::path binDirectory();

    /**
     * @brief The directory where data files are stored.
     */
    [[nodiscard]] static std::filesystem::path dataDirectory();

    /**
     * @brief The directory holding the web interface files.
     *
     * On Windows this is derived from the running executable, because the installation directory
     * is chosen when the installer runs and is not known at build time. Elsewhere the install
     * prefix is fixed by the build.
     */
    [[nodiscard]] static std::filesystem::path webfaceDirectory();

    /**
     * @brief The directory where configuration files are stored.
     */
    [[nodiscard]] static std::filesystem::path confDirectory();

    /**
     * @brief The system temp directory.
     */
    [[nodiscard]] static std::filesystem::path tempDirectory();

    /**
     * @brief The certificates directory.
     */
    [[nodiscard]] static std::filesystem::path certsDirectory();

    /**
     * @brief Put the certificates directory somewhere else than where it is installed.
     *
     * For tests. Installing a certificate writes real files, and the directory it would write
     * them into on a developer's machine belongs to that machine's own installation.
     *
     * @param directory         Directory to use, or empty to go back to the installed one.
     */
    static void setCertsDirectory(const std::filesystem::path& directory);

    /**
     * @brief Put the web interface files somewhere else than where they are installed.
     *
     * For tests, and for the same reason as setCertsDirectory() above: the installed directory is
     * the machine's, not the suite's. A test that asks the web interface for one of its own pages
     * would otherwise be answering out of whatever copy happens to be installed - or, on a machine
     * where XMQ never has been, out of nothing at all, which reads as a 404 and looks like a bug
     * in the server.
     *
     * @param directory         Directory to use, or empty to go back to the installed one.
     */
    static void setWebfaceDirectory(const std::filesystem::path& directory);

    /**
     * @brief The program logs directory.
     */
    [[nodiscard]] static std::filesystem::path logsDirectory();

    /**
     * @brief The SQLite connection URI naming this file, in the one form that works everywhere.
     *
     * SPTK reads everything after the first slash of the path as the file name, so the absolute
     * path has to keep its own leading slash and the URI carries two:
     *
     *     /var/lib/xmq/users.db   ->  sqlite3://localhost//var/lib/xmq/users.db
     *     C:\xmq\users.db          ->  sqlite3://localhost/C:\xmq\users.db
     *
     * Written once because getting it wrong is silent. With a single slash the Unix form names a
     * *relative* file: SQLite creates an empty database beside whatever the working directory
     * happens to be, the broker starts, and nobody can authenticate against a store that looks
     * present and is not. A Windows path had no working spelling at all until SPTK 5.6.10 - the
     * drive letter ran into the host name and the URI did not parse.
     */
    [[nodiscard]] static sptk::String sqliteUri(const std::filesystem::path& file);
};

} // namespace xmq
