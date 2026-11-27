#include "tui.h"

#include <cerrno>
#include <cstring>

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

using namespace std;

namespace tui {

namespace {

volatile sig_atomic_t g_terminate = 0;
volatile sig_atomic_t g_resized = 0;

termios g_savedTerminal {};
bool    g_rawMode = false;
bool    g_alternateScreen = false;

void onSignal(int)
{
    g_terminate = 1;
}

void onResize(int)
{
    g_resized = 1;
}

} // namespace

volatile sig_atomic_t& Terminal::terminateFlag()
{
    return g_terminate;
}

void restoreScreen()
{
    // Only async-signal-safe things here: this is called from the handler as well as from the
    // destructor, and a form must never leave the terminal with echo off and no cursor.
    if (g_rawMode)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTerminal);
        g_rawMode = false;
    }
    static constexpr char restore[] = "\033[?25h\033[?1049l";
    if (g_alternateScreen && ::write(STDOUT_FILENO, restore, sizeof(restore) - 1) < 0)
    {
        // Nothing useful to do about a failed write from a signal handler.
    }
    g_alternateScreen = false;
}

bool Terminal::available()
{
    return isatty(STDIN_FILENO) != 0;
}

Terminal::Terminal()
{
    if (!isatty(STDIN_FILENO))
    {
        return;
    }
    if (tcgetattr(STDIN_FILENO, &g_savedTerminal) == 0)
    {
        termios raw = g_savedTerminal;
        // Byte-at-a-time, no echo, no line editing: the form draws what was typed itself, and a
        // key must arrive when it is pressed rather than when Enter follows it.
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
        raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
        {
            g_rawMode = true;
        }
    }

    signal(SIGTERM, onSignal);
    signal(SIGINT, onSignal);
    signal(SIGHUP, onSignal);
    signal(SIGWINCH, onResize);

    const string sequence = "\033[?1049h\033[?25l\033[H\033[2J";
    if (::write(STDOUT_FILENO, sequence.data(), sequence.size()) < 0)
    {
        return;
    }
    g_alternateScreen = true;
}

Terminal::~Terminal()
{
    restoreScreen();
}

int Terminal::width() const
{
    winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
    {
        return size.ws_col;
    }
    return 80;
}

int Terminal::height() const
{
    winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0)
    {
        return size.ws_row;
    }
    return 24;
}

bool Terminal::takeResized()
{
    if (g_resized == 0)
    {
        return false;
    }
    g_resized = 0;
    return true;
}

void Terminal::clear()
{
    const string sequence = "\033[H\033[2J";
    write(sequence);
}

void Terminal::moveTo(const int row, const int column)
{
    // One-based, as every terminal expects, and clamped so a form drawn beyond the window shows a
    // blank line rather than wrapping into the field below it.
    const auto r = row < 1 ? 1 : row;
    const auto c = column < 1 ? 1 : column;
    write("\033[" + to_string(r) + ";" + to_string(c) + "H");
}

void Terminal::write(const string& text)
{
    if (text.empty())
    {
        return;
    }
    size_t written = 0;
    while (written < text.size())
    {
        const auto result = ::write(STDOUT_FILENO, text.data() + written, text.size() - written);
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return;
        }
        written += static_cast<size_t>(result);
    }
}

void Terminal::flush() const
{
    // write() above is unbuffered, so there is nothing to flush; the method exists so that a
    // caller can say when a frame is complete.
}

namespace {

/// Reads one byte, or -1 when the timeout expires.
int readByte(const int timeoutMs)
{
    pollfd input {STDIN_FILENO, POLLIN, 0};
    const auto ready = poll(&input, 1, timeoutMs);
    if (ready <= 0)
    {
        return -1;
    }
    unsigned char byte = 0;
    if (read(STDIN_FILENO, &byte, 1) != 1)
    {
        return -1;
    }
    return byte;
}

} // namespace

KeyPress Terminal::readKey(const int timeoutMs)
{
    KeyPress press;

    auto byte = readByte(timeoutMs);
    if (byte < 0)
    {
        return press;
    }

    switch (byte)
    {
        case '\r':
        case '\n':
            press.key = Key::Enter;
            return press;
        case '\t':
            press.key = Key::Tab;
            return press;
        case 127:
        case 8:
            press.key = Key::Backspace;
            return press;
        case 27:
            break; // Possibly the start of a sequence; decided below.
        default:
            if (byte < 32)
            {
                return press;
            }
            break;
    }

    if (byte != 27)
    {
        // One UTF-8 code point, so a Cyrillic letter in a note arrives whole.
        string text(1, static_cast<char>(byte));
        const auto expected = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : byte >= 0xC0 ? 2 : 1;
        for (int index = 1; index < expected; ++index)
        {
            const auto next = readByte(20);
            if (next < 0)
            {
                break;
            }
            text += static_cast<char>(next);
        }
        press.key = Key::Character;
        press.text = text;
        return press;
    }

    // ESC: on its own it is the Escape key, followed by '[' or 'O' it introduces a sequence. The
    // short wait is what tells the two apart on a terminal that sends a bare ESC.
    const auto second = readByte(30);
    if (second < 0)
    {
        press.key = Key::Escape;
        return press;
    }
    if (second != '[' && second != 'O')
    {
        return press;
    }

    const auto third = readByte(30);
    if (third < 0)
    {
        return press;
    }

    switch (third)
    {
        case 'A':
            press.key = Key::Up;
            return press;
        case 'B':
            press.key = Key::Down;
            return press;
        case 'C':
            press.key = Key::Right;
            return press;
        case 'D':
            press.key = Key::Left;
            return press;
        case 'H':
            press.key = Key::Home;
            return press;
        case 'F':
            press.key = Key::End;
            return press;
        case 'Z':
            press.key = Key::BackTab;
            return press;
        case 'P':
            press.key = Key::F1;
            return press;
        case 'Q':
            press.key = Key::F2;
            return press;
        case 'R':
            press.key = Key::F3;
            return press;
        case 'S':
            press.key = Key::F4;
            return press;
        default:
            break;
    }

    if (third < '0' || third > '9')
    {
        return press;
    }

    // A number and then '~': xterm's spelling of the keys that have no letter code.
    string number(1, static_cast<char>(third));
    while (true)
    {
        const auto next = readByte(20);
        if (next < '0' || next > '9')
        {
            break;
        }
        number += static_cast<char>(next);
    }

    if (number == "1" || number == "7")
    {
        press.key = Key::Home;
    }
    else if (number == "4" || number == "8")
    {
        press.key = Key::End;
    }
    else if (number == "3")
    {
        press.key = Key::Delete;
    }
    else if (number == "5")
    {
        press.key = Key::PageUp;
    }
    else if (number == "6")
    {
        press.key = Key::PageDown;
    }
    else if (number == "14")
    {
        press.key = Key::F4;
    }
    else if (number == "15")
    {
        press.key = Key::F5;
    }
    else if (number == "19")
    {
        press.key = Key::F8;
    }
    else if (number == "21")
    {
        press.key = Key::F10;
    }
    return press;
}

int Terminal::displayWidth(const string& text)
{
    int columns = 0;
    for (size_t at = 0; at < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[at]);
        const auto length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        at += length;
        ++columns;
    }
    return columns;
}

size_t Terminal::lastCodePointBytes(const string& text)
{
    if (text.empty())
    {
        return 0;
    }
    size_t at = text.size();
    // Walk back over continuation bytes (10xxxxxx) to the start of the code point.
    while (at > 0 && (static_cast<unsigned char>(text[at - 1]) & 0xC0) == 0x80)
    {
        --at;
    }
    if (at > 0)
    {
        --at;
    }
    return text.size() - at;
}

} // namespace tui
