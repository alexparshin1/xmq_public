/**
 * A framed dialog, drawn once and reused by every screen that asks a question.
 *
 * The frame, its shadow, the text column and especially the buttons live here rather than in each
 * caller: a button's shadow has to have exactly the button's width, and a label with one space
 * more or less than the last one used to leave the shadow sticking out past it. The buttons are
 * measured from their visible labels (the '~' shortcuts do not count), all of them are given the
 * widest one's width, and the row is centred in the frame.
 *
 * Content is drawn in rows relative to the first text line, so a caller does not repeat the frame
 * geometry: row 0 is just under the title, and the buttons are always in the same place above the
 * bottom border.
 */
#pragma once

#include "frame.h"
#include "theme.h"

#include <string>
#include <vector>

namespace tui {

class Dialog final
{
public:
    /**
     * @param frame        The screen to draw on; the constructor draws the frame and shadow.
     * @param screenWidth  Terminal width, so the dialog can centre itself.
     * @param screenHeight Terminal height.
     * @param width        Requested width, clamped to the screen.
     * @param height       Requested height, clamped to the screen.
     * @param title        The title in the top border.
     * @param buttons      Button labels, left to right; '~' marks the shortcut.
     */
    Dialog(Frame& frame, int screenWidth, int screenHeight, int width, int height,
           std::string title, std::vector<std::string> buttons);

    [[nodiscard]] int left() const { return m_left; }
    [[nodiscard]] int top() const { return m_top; }
    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }

    /// First column and first row available to text, inside the border.
    [[nodiscard]] int contentLeft() const { return m_left + 3; }
    [[nodiscard]] int contentTop() const { return m_top + 2; }
    [[nodiscard]] int contentWidth() const { return m_width - 6; }

    /// A line of text, relative to contentTop() and padded to the content width.
    void line(int row, const std::string& text, const char* color = theme::dialog);

    /// A label with an input box under it, relative to contentTop().
    void field(int row, const std::string& label, const std::string& value, bool active);

    /// An input box alone, with its value clipped from the left so the caret end stays visible.
    void input(int row, const std::string& value, bool active);

    /// Draws the buttons in their row above the bottom border; @param selected is the active one.
    void drawButtons(int selected);

private:
    void drawFrame();

    Frame&                   m_frame;
    int                      m_left {1};
    int                      m_top {1};
    int                      m_width {10};
    int                      m_height {5};
    std::string              m_title;
    std::vector<std::string> m_buttons;
};

} // namespace tui
