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

#include "ProgressBar.h"

#include <sptk5/Printer.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sptk5/Buffer.h>

using namespace std;
using namespace std::chrono;
using namespace xmq;

namespace {
constexpr int          barWidth = 30;
constexpr milliseconds minRedrawInterval {100};
constexpr size_t       minRedrawProgress {100};
} // namespace

ProgressBar::ProgressBar(string label, const bool enabled)
    : m_label(std::move(label))
    , m_enabled(enabled)
{
}

string ProgressBar::bar(const size_t filledSize, const size_t emptySize)
{
    string bar;

#ifdef _WIN32
    string fill("#");
    string empty("-");
#else
    const string fill("█");
    const string empty("░");
#endif
    for (size_t i = 0; i < filledSize; ++i)
    {
        bar += fill;
    }
    for (size_t i = 0; i < emptySize; ++i)
    {
        bar += empty;
    }
    return bar;
}

void ProgressBar::update(const size_t current, const size_t total)
{
    if (!m_enabled || total == 0)
    {
        return;
    }

    const auto current_ = min(current, total);
    const auto isDone = current_ == total;

    const auto now = steady_clock::now();
    if (m_printed && !isDone && current_ >= m_lastPrintedCount &&
        (current_ - m_lastPrintedCount < minRedrawProgress || now - m_lastPrint < minRedrawInterval))
    {
        return;
    }
    m_lastPrint = now;
    m_lastPrintedCount = current_;
    m_printed = true;

    const auto fraction = static_cast<double>(current_) / static_cast<double>(total);
    const auto filled = static_cast<int>(fraction * barWidth);

    const scoped_lock printLock(sptk::Console::printMutex());
    cout << '\r' << setw(28) << m_label << " [" << bar(filled, barWidth - filled) << "] "
         << setw(3) << static_cast<int>(fraction * 100) << "% (" << current_ << '/' << total << ')' << "  " << flush;
}

void ProgressBar::finish()
{
    if (!m_enabled || !m_printed)
    {
        return;
    }

    const scoped_lock printLock(sptk::Console::printMutex());
    cout << '\n'
         << flush;
    m_printed = false;
}
