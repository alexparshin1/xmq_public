/**
 * A terminal, as much of one as a form needs: raw mode, a size, and keys with a timeout.
 *
 * Kept apart from the screen that uses it so that the platform-specific part - termios here, and
 * the console API if this ever has to build on Windows - is in one place instead of spread through
 * the form's drawing code.
 */
#pragma once

#include <csignal>
#include <cstddef>
#include <string>

namespace tui {

enum class Key
{
    None,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Delete,
    Backspace,
    Enter,
    Tab,
    BackTab,
    Escape,
    F1,
    F2,
    F3,
    F5,
    F8,
    F10,
    Character, ///< A printable character; see KeyPress::text.
};

struct KeyPress
{
    Key             key {Key::None};
    std::string     text; ///< The bytes the character key produced, UTF-8.
};

/**
 * The terminal in raw mode, on the alternate screen.
 *
 * Constructing it switches both on and destroying it switches them back, including on exit through
 * a signal: the form's own loop reads a flag set by the handler, so a SIGTERM from screen or a
 * dropped ssh session does not leave the terminal with echo off.
 */
class Terminal final
{
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /// @return true if standard input is a terminal. Without one the form refuses to open rather
    ///         than drawing escape codes into a pipe.
    [[nodiscard]] static bool available();

    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;

    /// @param timeoutMs How long to wait for a key before returning Key::None, so the caller can
    ///                  notice a resize or a signal while nobody is typing.
    [[nodiscard]] KeyPress readKey(int timeoutMs);

    /// True once a resize has happened since the last call.
    [[nodiscard]] bool takeResized();

    void clear();
    void moveTo(int row, int column);
    void write(const std::string& text);
    void flush() const;

    /// Columns a UTF-8 string occupies. Cyrillic and ASCII text are one column per code point;
    /// wide East Asian characters are not handled, and the form keeps to single-width text.
    [[nodiscard]] static int displayWidth(const std::string& text);

    /// Number of bytes in the last code point of a UTF-8 string, so a backspace removes a letter
    /// rather than one of its bytes.
    [[nodiscard]] static size_t lastCodePointBytes(const std::string& text);

    /// Set from a signal handler; the form's loop reads it and returns through the normal path.
    static volatile sig_atomic_t& terminateFlag();
};

/// Forgets everything the screen showed and puts the cursor back, called on the normal exit path
/// and by the signal handler.
void restoreScreen();

} // namespace tui
