/**
 * The screen as a grid of cells, so a redraw can send only what changed.
 *
 * A one-second full repaint is a whole screen of spaces followed by the same screen again, and over
 * ssh through screen that is visible as a blink even when nothing moved. Composing each frame in
 * memory first turns "repaint" into "send the two lines that are different", which is nothing at
 * all on a quiet second and a handful of bytes on a busy one.
 *
 * Both screens use it: the viewer repaints on a timer, the form on every key, and a form that
 * rewrites everything on each keypress flickers worse than the viewer did.
 */
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace tui {

using std::string;
using std::to_string;
using std::vector;

class Frame final
{
public:
    void resize(const int width, const int height)
    {
        m_width = width;
        m_height = height;
        m_cells.assign(static_cast<size_t>(height * width), Cell {});
        if (static_cast<int>(m_rendered.size()) != height)
        {
            m_rendered.assign(static_cast<size_t>(height), string {});
        }
    }

    void clear(const char* color)
    {
        const auto id = colorId(color);
        for (auto& cell: m_cells)
        {
            cell.glyph = " ";
            cell.color = id;
            cell.foreground = -1;
        }
    }

    /// @param row One-based row, @param column one-based column, as the terminal counts them.
    void put(const int row, const int column, const char* color, const string& text)
    {
        if (row < 1 || row > m_height || column < 1 || column > m_width)
        {
            return;
        }
        const auto id = colorId(color);
        int        at = column - 1;
        for (size_t index = 0; index < text.size() && at < m_width;)
        {
            const auto lead = static_cast<unsigned char>(text[index]);
            const auto length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            auto&      cell = m_cells[static_cast<size_t>((row - 1) * m_width + at)];
            cell.glyph = text.substr(index, length);
            cell.color = id;
            cell.foreground = -1;
            index += length;
            ++at;
        }
    }

    /// Repeat a single-cell glyph with a new foreground, keeping each cell's existing background.
    void overlay(const int row, const int column, const int count, const char* foreground, const string& glyph)
    {
        if (row < 1 || row > m_height || column < 1 || column > m_width)
        {
            return;
        }
        const auto id = colorId(foreground);
        for (int i = 0; i < count && column + i <= m_width; ++i)
        {
            auto& cell = m_cells[static_cast<size_t>((row - 1) * m_width + column + i - 1)];
            cell.glyph = glyph;
            cell.foreground = id;
        }
    }

    /// Sends the rows that differ from the last frame. Nothing on a quiet second.
    void flush()
    {
        string output;
        for (int row = 1; row <= m_height; ++row)
        {
            const auto rendered = renderRow(row);
            if (rendered == m_rendered[static_cast<size_t>(row - 1)])
            {
                continue;
            }
            m_rendered[static_cast<size_t>(row - 1)] = rendered;
            output += "\033[" + to_string(row) + ";1H" + rendered;
        }
        if (!output.empty())
        {
            output += "\033[?25l";
            fwrite(output.data(), 1, output.size(), stdout);
            fflush(stdout);
        }
    }

private:
    struct Cell
    {
        string glyph {" "};
        int    color {0};
        int    foreground {-1};
    };

    int colorId(const char* color)
    {
        for (size_t index = 0; index < m_palette.size(); ++index)
        {
            if (m_palette[index] == color)
            {
                return static_cast<int>(index);
            }
        }
        m_palette.push_back(color);
        return static_cast<int>(m_palette.size()) - 1;
    }

    string renderRow(const int row)
    {
        string text;
        int    currentColor = -1;
        int    currentForeground = -1;
        for (int column = 0; column < m_width; ++column)
        {
            const auto& cell = m_cells[static_cast<size_t>((row - 1) * m_width + column)];
            if (cell.color != currentColor || cell.foreground != currentForeground)
            {
                text += m_palette[static_cast<size_t>(cell.color)];
                if (cell.foreground >= 0)
                {
                    text += m_palette[static_cast<size_t>(cell.foreground)];
                }
                currentColor = cell.color;
                currentForeground = cell.foreground;
            }
            text += cell.glyph;
        }
        text += "\033[0m";
        return text;
    }

    int                        m_width {0};
    int                        m_height {0};
    vector<Cell>               m_cells;
    vector<string>             m_rendered;
    vector<const char*>        m_palette;
};

/// Columns a UTF-8 string occupies, enough for text this program draws itself.
inline int columnsOf(const std::string& text)
{
    int    columns = 0;
    size_t at = 0;
    while (at < text.size())
    {
        at += 1;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
        {
            ++at;
        }
        ++columns;
    }
    return columns;
}

/// Draws text in which the segments between '~' are the shortcut, in `hot`: the way a Turbo Vision
/// screen showed which letter a dialog item answered to, and which keys the status line offered.
inline void putMarked(Frame& frame, const int row, const int column, const char* normal, const char* hot,
                      const std::string& text)
{
    int    at = column;
    bool   highlighted = false;
    std::string run;
    const auto flush = [&]
    {
        if (run.empty())
        {
            return;
        }
        frame.put(row, at, highlighted ? hot : normal, run);
        at += columnsOf(run);
        run.clear();
    };
    for (const auto character: text)
    {
        if (character == '~')
        {
            flush();
            highlighted = !highlighted;
            continue;
        }
        run += character;
    }
    flush();
}

} // namespace tui
