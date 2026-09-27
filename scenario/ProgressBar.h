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

#include <chrono>
#include <cstddef>
#include <string>

namespace xmq {

/**
 * @brief Single-line, in-place progress bar for long-running CLI operations.
 *
 * Redraws itself in place (using a carriage return, no trailing newline) each time update()
 * is called, but rate-limits actual redraws so a caller in a tight per-message loop doesn't
 * thrash the terminal. A disabled bar (enabled == false) is a no-op, so call sites don't need
 * to branch on whether the progress bar was requested.
 */
class ProgressBar
{
public:
    /**
     * @brief Constructor.
     * @param label     Text shown before the bar, e.g. "Connecting publishers".
     * @param enabled   When false, update() and finish() are no-ops.
     */
    ProgressBar(std::string label, bool enabled);

    /**
     * @brief Redraw the bar to reflect current/total progress.
     * A redraw at less than 100% is skipped if the last one happened too recently.
     * The 100%-complete redraw is never skipped, so the bar always ends up showing completion.
     * @param current   Completed unit count so far.
     * @param total     Total expected unit count. A total of 0 means nothing to show; skipped.
     */
    void update(size_t current, size_t total);

    /**
     * @brief Move the cursor past the bar, printing a newline once, iff a bar line was drawn.
     */
    void finish();

private:
    std::string                           m_label;
    bool                                  m_enabled;
    bool                                  m_printed {false};
    std::chrono::steady_clock::time_point m_lastPrint {};

    /**
     * @brief Return the string that represents the bar.
     * @param filledSize The filled part of the bar.
     * @param emptySize The empty part of the bar.
     * @return
     */
    static std::string bar(size_t filledSize, size_t emptySize);
};

} // namespace xmq
