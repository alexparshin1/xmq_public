#include "dialog.h"

#include <algorithm>

using namespace std;

namespace tui {

namespace {

/// Columns a label takes on screen: the '~' that marks a shortcut is not drawn.
int visibleColumns(const string& label)
{
    string plain;
    plain.reserve(label.size());
    for (const auto character: label)
    {
        if (character != '~')
        {
            plain += character;
        }
    }
    return columnsOf(plain);
}

/// @param text A UTF-8 glyph repeated `count` times.
string repeated(const string& text, const int count)
{
    string result;
    for (int i = 0; i < count; ++i)
    {
        result += text;
    }
    return result;
}

/// The first code point, so an over-long value loses whole UTF-8 sequences.
size_t firstCodePointBytes(const string& text)
{
    size_t at = 1;
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
    {
        ++at;
    }
    return at;
}

} // namespace

Dialog::Dialog(Frame& frame, const int screenWidth, const int screenHeight, int width, int height,
               string title, vector<string> buttons)
    : m_frame(frame),
      m_title(std::move(title)),
      m_buttons(std::move(buttons))
{
    m_width = clamp(width, 10, max(screenWidth, 10));
    m_height = clamp(height, 5, max(screenHeight, 5));
    m_left = max(1, (screenWidth - m_width) / 2 + 1);
    m_top = max(1, (screenHeight - m_height) / 2 + 1);
    drawFrame();
}

void Dialog::drawFrame()
{
    const int right = m_left + m_width;
    const int shadowBottom = m_top + m_height;
    for (int row = m_top + 1; row < shadowBottom; ++row)
    {
        m_frame.overlay(row, right, 1, theme::windowShadow, "▌");
    }
    m_frame.overlay(shadowBottom, m_left + 2, m_width - 2, theme::windowShadow, "▀");
    m_frame.overlay(shadowBottom, right, 1, theme::windowShadow, "▘");

    string topBorder {"╔"};
    string bottomBorder {"╚"};
    for (int i = 0; i < m_width - 2; ++i)
    {
        topBorder += "═";
        bottomBorder += "═";
    }
    m_frame.put(m_top, m_left, theme::dialogFrame, topBorder + "╗");
    m_frame.put(m_top + m_height - 1, m_left, theme::dialogFrame, bottomBorder + "╝");
    for (int row = m_top + 1; row < m_top + m_height - 1; ++row)
    {
        m_frame.put(row, m_left, theme::dialogFrame, "║");
        m_frame.put(row, m_left + m_width - 1, theme::dialogFrame, "║");
        m_frame.put(row, m_left + 1, theme::dialog, string(static_cast<size_t>(m_width - 2), ' '));
    }
    m_frame.put(m_top, m_left + 2, theme::dialogFrame, "[ ]");
    m_frame.put(m_top, m_left + 3, theme::dialogAccent, "■");
    const int titleLeft = m_left + max(2, (m_width - columnsOf(m_title)) / 2);
    m_frame.put(m_top, titleLeft, theme::dialogFrame, m_title);
}

void Dialog::line(const int row, const string& text, const char* color)
{
    if (row < 0)
    {
        return;
    }
    m_frame.put(m_top + 2 + row, m_left + 3, color, padded(text, contentWidth()));
}

void Dialog::field(const int row, const string& label, const string& value, const bool active)
{
    line(row, label, theme::dialog);
    input(row + 1, value, active);
}

void Dialog::input(const int row, const string& value, const bool active)
{
    const int columns = contentWidth();
    if (columns <= 0 || row < 0)
    {
        return;
    }
    auto visible = value;
    const auto room = max(0, columns - 2);
    while (columnsOf(visible) > room)
    {
        visible.erase(0, firstCodePointBytes(visible));
    }
    const auto color = active ? theme::selected : theme::input;
    const auto screenRow = m_top + 2 + row;
    m_frame.put(screenRow, m_left + 3, color, string(static_cast<size_t>(columns), ' '));
    m_frame.put(screenRow, m_left + 4, color, visible);
}

void Dialog::drawButtons(const int selected)
{
    if (m_buttons.empty())
    {
        return;
    }

    auto labelWidth = 0;
    for (const auto& label: m_buttons)
    {
        labelWidth = max(labelWidth, visibleColumns(label));
    }
    const int width = labelWidth + 2;
    const int gap = 3;
    const auto total = static_cast<int>(m_buttons.size()) * width +
                       (static_cast<int>(m_buttons.size()) - 1) * gap;
    const int row = m_top + m_height - 3;
    int       column = m_left + max(1, (m_width - total) / 2);

    for (size_t index = 0; index < m_buttons.size(); ++index)
    {
        const auto& label = m_buttons[index];
        const auto visible = visibleColumns(label);
        const auto leading = max(0, (width - visible) / 2);
        const auto trailing = max(0, width - visible - leading);
        const auto text = string(static_cast<size_t>(leading), ' ') + label +
                          string(static_cast<size_t>(trailing), ' ');

        // The shadow is one row down and one column right, exactly as wide as the button, with
        // the half block at the button's right end joining it to the button.
        m_frame.put(row + 1, column + 1, theme::buttonShadowBelow, repeated("▀", width));
        m_frame.put(row, column + width, theme::dialog, "▄");
        putMarked(m_frame, row, column, static_cast<int>(index) == selected ? theme::buttonActive : theme::button,
                  theme::buttonHot, text);
        column += width + gap;
    }
}

} // namespace tui
