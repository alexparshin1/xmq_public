/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "base/xmq.h"

#include <memory>
#include <string>

#include <sptk5/LogEngine.h>

namespace xmq {

/**
 * @brief A log engine that writes one JSON object per line.
 *
 * It sits in front of the real engine rather than replacing it: the file is still a FileLogEngine,
 * still appended to between restarts, still rotated the same way. What changes is the line.
 *
 * The fields XMQ already knows structurally are extracted from the message rather than guessed
 * from English: `ServerData::logMessage()` puts the subject in `&lt;subject&gt;`, the node comes
 * from the logger prefix `(node)`, and the remaining direct loggers put their component in
 * `[Component]`. Anything else stays in `message`, so no line is lost when it does not match.
 *
 * The file engine underneath is given only ENABLE: its DATE/TIME/PRIORITY prefixes would make the
 * line invalid JSON. The wrapper's own STDOUT option prints the same JSON to the console.
 */
class XMQ_EXPORT JsonLogEngine final : public sptk::LogEngine
{
public:
    /**
     * @brief Constructor.
     * @param inner     Engine that writes the formatted line (a FileLogEngine).
     * @param nodeName  This node's name, for records that do not carry one.
     */
    JsonLogEngine(std::shared_ptr<sptk::LogEngine> inner, std::string nodeName);

    ~JsonLogEngine() override;

    JsonLogEngine(const JsonLogEngine&) = delete;
    JsonLogEngine& operator=(const JsonLogEngine&) = delete;

    /// Whether the formatted line is also written to stdout. The base engine's own STDOUT option
    /// is not used: it would print the original text, not the JSON.
    void setStdout(bool enabled)
    {
        m_stdout = enabled;
    }

    bool saveMessage(const sptk::Logger::Message& message) override;
    void flush() override;
    void reset() override;
    std::filesystem::path rotate(size_t keepArchives) override;

private:
    std::shared_ptr<sptk::LogEngine> m_inner;
    std::string                      m_nodeName;
    bool                             m_stdout {false};

    [[nodiscard]] std::string format(const sptk::Logger::Message& message) const;
};

} // namespace xmq
