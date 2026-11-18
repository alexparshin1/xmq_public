#include "run_form.h"

#include "frame.h"
#include "theme.h"
#include "tui.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

using namespace std;
namespace fs = std::filesystem;

namespace stand {

namespace {

enum class Kind
{
    Text,
    Choice,
    Toggle,
};

struct Field
{
    string         label;
    string         help;
    Kind           kind {Kind::Text};
    string*        value {nullptr};
    bool*          flag {nullptr};
    vector<string> choices;
    const char*    placeholder {nullptr}; ///< Shown while the value is empty; not a value itself.
};

class Form final
{
public:
    explicit Form(RunSpec& spec)
        : m_spec(spec)
    {
        build();
    }

    FormAction run()
    {
        while (!m_done)
        {
            draw();
            const auto press = m_terminal.readKey(200);
            if (tui::Terminal::terminateFlag() != 0)
            {
                return FormAction::Cancelled;
            }
            if (m_terminal.takeResized())
            {
                continue;
            }
            handle(press);
        }
        return m_saved ? FormAction::Saved : FormAction::Cancelled;
    }

private:
    void build()
    {
        static const vector<string> yesNo {"no", "yes"};

        // A starting value rather than an empty line: the newest version with published results is
        // what the next set is usually about, and the version also names the default run file.
        if (m_spec.version.empty())
        {
            m_spec.version = suggestedVersion();
        }

        vector<string> sets = availableScenarioSets();
        if (find(sets.begin(), sets.end(), m_spec.setFile) == sets.end() && !m_spec.setFile.empty())
        {
            sets.push_back(m_spec.setFile);
        }
        if (sets.empty())
        {
            sets.push_back("Scenario-Set-Bench.txt");
        }

        m_fields.push_back({"Version", "XMQ version under test; it names the bench profile to restart", Kind::Text, &m_spec.version, nullptr, {}});
        m_fields.push_back({"Set", "Scenario set to run, from the *.txt files next to stand_run.sh", Kind::Choice, &m_spec.setFile, nullptr, sets});
        m_fields.push_back({"Server", "Broker host under test", Kind::Text, &m_spec.server, nullptr, {}});
        m_fields.push_back({"Nic", "Client NIC watched for dropped packets", Kind::Text, &m_spec.nic, nullptr, {}});
        m_fields.push_back({"Subnet", "Client source subnet", Kind::Text, &m_spec.subnet, nullptr, {}});
        m_fields.push_back({"First", "First host octet in that subnet", Kind::Text, &m_spec.firstOctet, nullptr, {}});
        m_fields.push_back({"Count", "Number of source addresses", Kind::Text, &m_spec.count, nullptr, {}});
        m_fields.push_back({"Out", "Per-scenario logs; empty means results/raw/<stamp>-xmq<version>-bench", Kind::Text, &m_spec.out, nullptr, {}, "(stand_run.sh default)"});
        m_fields.push_back({"Run file", "Where to write this run file", Kind::Text, &m_runFilePath, nullptr, {}});
        m_fields.push_back({"File results", "File the record with file_results.py when the set succeeds", Kind::Toggle, nullptr, &m_spec.fileResults, yesNo});
        m_fields.push_back({"Keep brokers", "Refuse to start if another broker owns the port, instead of stopping it", Kind::Toggle, nullptr, &m_spec.keepOtherBrokers, yesNo});
        m_fields.push_back({"Skip preflight", "Do not run preflight.sh on either host", Kind::Toggle, nullptr, &m_spec.skipPreflight, yesNo});
        m_fields.push_back({"Note", "Free-text line for the record", Kind::Text, &m_spec.note, nullptr, {}});

        m_runFilePath = displayPath(m_spec.file.empty() ? defaultRunFile(m_spec) : m_spec.file);
    }

    [[nodiscard]] string shownValue(const Field& field) const
    {
        if (field.kind == Kind::Toggle)
        {
            return *field.flag ? "[X]" : "[ ]";
        }
        const string value = *field.value;
        if (value.empty() && field.placeholder != nullptr)
        {
            return field.placeholder;
        }
        return value;
    }

    /// Paints the screen the way a Turbo Vision application painted a dialog: the dialog fills the
    /// screen, its own frame at the edges, buttons inside the bottom edge and the keys on a status
    /// bar. The frame is composed in memory and only the lines that changed are sent, so moving
    /// between fields does not repaint the whole screen - the same reason the viewer stopped
    /// blinking.
    void draw()
    {
        const auto width = m_terminal.width();
        const auto height = m_terminal.height();

        m_frame.resize(width, height);
        m_frame.clear(theme::desktop);

        // Edge to edge: the dialog fills the screen, as the window in a Turbo Vision application did
        // once it was maximised. A centred box wastes the width the values need - the paths are
        // long - and the edges are the only boundaries worth having.
        const auto left = 1;
        const auto innerWidth = max(30, width - 2);
        const auto contentLeft = left + 2;
        const auto labelWidth = 15;
        const auto valueLeft = contentLeft + labelWidth;
        const auto valueWidth = max(8, innerWidth - labelWidth - 2);

        const auto top = 1;
        const auto contentTop = top + 2;
        const auto rows = static_cast<int>(m_fields.size());
        const auto helpRow = contentTop + rows;
        const auto problemRow = helpRow + 1;
        // Keep the last row available for the status bar; on a short terminal the dialog body is
        // clipped rather than pushing its keyboard instructions off-screen.
        const auto bottom = min(height - 1, max(helpRow + 4, height - 1));
        const auto buttonRow = bottom - 1;

        const auto put = [this](const int row, const int column, const char* color, const string& text)
        {
            m_frame.put(row, column, color, text);
        };
        const auto blank = [](const size_t count)
        {
            return string(count, ' ');
        };
        // Clipping by display width, not by bytes: a long path is clipped where the dialog ends
        // instead of pushing the field below it off the frame.
        const auto clip = [](const string& text, const int columns)
        {
            if (columns <= 0)
            {
                return string {};
            }
            int    used = 0;
            size_t at = 0;
            while (at < text.size() && used < columns)
            {
                at += 1;
                while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
                {
                    ++at;
                }
                ++used;
            }
            return text.substr(0, at);
        };
        const auto fill = [&](const int row, const int column, const int count, const char* color)
        {
            put(row, column, color, blank(static_cast<size_t>(max(0, count))));
        };

        // The dialog body, inside its frame, then the frame itself: single line, because Turbo
        // Vision drew dialogs with one line and windows with two.
        for (int row = top + 1; row < bottom; ++row)
        {
            fill(row, left + 1, innerWidth, theme::desktop);
        }

        string topLine {"┌"};
        string bottomLine {"└"};
        const string singleLine {"─"};
        for (int column = 0; column < innerWidth; ++column)
        {
            topLine += singleLine;
            bottomLine += singleLine;
        }
        topLine += "┐";
        bottomLine += "┘";
        put(top, left, theme::frame, topLine);
        put(bottom, left, theme::frame, bottomLine);
        for (int row = top + 1; row < bottom; ++row)
        {
            put(row, left, theme::frame, "│");
            put(row, left + innerWidth + 1, theme::frame, "│");
        }

        const auto title = m_spec.file.empty() ? string {"stand_runner --edit"}
                                               : "stand_runner --edit  " + displayPath(m_spec.file);
        const auto titleText = " " + clip(title, innerWidth - 10) + " ";
        put(top, left + 4 + max(0, (innerWidth - 6 - static_cast<int>(titleText.size())) / 2), theme::frameActive, titleText);
        put(top, left + 1, theme::frameActive, "[■]");

        // One line per field: the label in the dialog's own colour with its first letter standing
        // out, and the value in an input line - white, which is what made a field look editable.
        for (int index = 0; index < rows; ++index)
        {
            const auto& field = m_fields[static_cast<size_t>(index)];
            const bool  selected = static_cast<size_t>(index) == m_current;
            const bool  active = selected && m_editing;
            const int   row = contentTop + index;

            fill(row, left + 1, innerWidth, theme::desktop);

            // The label is a name, not a shortcut: this screen has no Alt+letter to answer to, so
            // marking the first letter would promise a key that does nothing.
            const auto label = field.label;
            put(row, contentLeft + 1, theme::desktop,
                label + blank(static_cast<size_t>(max(0, labelWidth - 2 - static_cast<int>(label.size())))));

            string value = shownValue(field);
            if (field.kind == Kind::Choice)
            {
                value = "< " + value + " >";
            }

            if (active)
            {
                // While editing, show the tail of a long value so the cursor stays on screen.
                const auto cursorColumns = tui::Terminal::displayWidth(value.substr(0, m_cursor));
                const auto start = static_cast<size_t>(max(0, cursorColumns - valueWidth + 1));
                const auto offset = [&value, start]
                {
                    int    columns = 0;
                    size_t at = 0;
                    while (at < value.size() && columns < static_cast<int>(start))
                    {
                        at += 1;
                        while (at < value.size() && (static_cast<unsigned char>(value[at]) & 0xC0) == 0x80)
                        {
                            ++at;
                        }
                        ++columns;
                    }
                    return at;
                }();
                const auto visible = clip(value.substr(offset), valueWidth - 1);
                put(row, valueLeft + 1, theme::selected,
                    " " + visible + blank(static_cast<size_t>(max(0, valueWidth - 1 - tui::Terminal::displayWidth(visible)))));
            }
            else if (selected)
            {
                const auto text = clip(value, valueWidth - 1);
                put(row, valueLeft + 1, theme::selected,
                    " " + text + blank(static_cast<size_t>(max(0, valueWidth - 1 - tui::Terminal::displayWidth(text)))));
            }
            else
            {
                const auto text = clip(value, valueWidth - 1);
                put(row, valueLeft + 1, theme::input,
                    " " + text + blank(static_cast<size_t>(max(0, valueWidth - 1 - tui::Terminal::displayWidth(text)))));
            }
        }

        // The help for the field the keyboard is on, then anything that was refused.
        {
            const auto help = clip(m_fields[m_current].help, innerWidth - 4);
            put(helpRow, contentLeft, theme::desktop, " " + help + blank(static_cast<size_t>(max(0, innerWidth - 4 - tui::Terminal::displayWidth(help)))));
        }
        for (size_t index = 0; index < m_problems.size() && problemRow + static_cast<int>(index) < buttonRow; ++index)
        {
            const auto text = clip(m_problems[index], innerWidth - 6);
            put(problemRow + static_cast<int>(index), contentLeft, theme::warningDesktop,
                " ! " + text + blank(static_cast<size_t>(max(0, innerWidth - 6 - tui::Terminal::displayWidth(text)))));
        }

        // No buttons inside the frame: F2 and Esc are named on the status bar, which is where a
        // Turbo Vision screen put the keys it offered. A second copy inside the dialog was one
        // place too many to keep in step.

        // The status bar, grey with red keys, always owns the final terminal row.
        fill(height, 1, width, theme::status);
        tui::putMarked(m_frame, height, 2, theme::status, theme::statusKey,
                       "~↑↓~ move   ~Enter~ edit   ~F2~ save   ~Esc~ leave without saving");

        m_frame.flush();

        if (m_editing)
        {
            const auto prefix = shownValue(m_fields[m_current]).substr(0, m_cursor);
            m_terminal.moveTo(contentTop + static_cast<int>(m_current),
                              valueLeft + 2 + tui::Terminal::displayWidth(prefix));
            m_terminal.write("\033[?25h");
        }
        else
        {
            m_terminal.write("\033[?25l");
        }
    }

    void handle(const tui::KeyPress& press)
    {
        if (m_editing)
        {
            handleEditing(press);
            return;
        }

        switch (press.key)
        {
            case tui::Key::Up:
                m_current = m_current == 0 ? m_fields.size() - 1 : m_current - 1;
                return;
            case tui::Key::Down:
            case tui::Key::Tab:
                m_current = (m_current + 1) % m_fields.size();
                return;
            case tui::Key::BackTab:
                m_current = m_current == 0 ? m_fields.size() - 1 : m_current - 1;
                return;
            case tui::Key::Enter:
                activate();
                return;
            case tui::Key::Character:
                // The space bar is what a hand reaches for on a check box; the first version only
                // answered to Enter, which is not what the hand expects. On a field it types a
                // space, because that is what a space does in a text field.
                if (press.text == " ")
                {
                    if (m_fields[m_current].kind == Kind::Text)
                    {
                        activate();
                        if (m_editing)
                        {
                            m_fields[m_current].value->insert(m_cursor, " ");
                            ++m_cursor;
                        }
                    }
                    else
                    {
                        step(1);
                    }
                }
                return;
            case tui::Key::Left:
                step(-1);
                return;
            case tui::Key::Right:
                step(1);
                return;
            case tui::Key::F2:
                save();
                return;
            case tui::Key::Escape:
                m_done = true;
                return;
            default:
                return;
        }
    }

    void handleEditing(const tui::KeyPress& press)
    {
        auto& field = m_fields[m_current];
        auto& value = *field.value;

        switch (press.key)
        {
            case tui::Key::Enter:
                commit();
                return;
            case tui::Key::Escape:
                value = m_editBackup;
                m_cursor = value.size();
                m_editing = false;
                return;
            case tui::Key::Tab:
                commit();
                m_current = (m_current + 1) % m_fields.size();
                return;
            case tui::Key::BackTab:
                commit();
                m_current = m_current == 0 ? m_fields.size() - 1 : m_current - 1;
                return;
            case tui::Key::Left:
                if (m_cursor > 0)
                {
                    --m_cursor;
                    while (m_cursor > 0 && (static_cast<unsigned char>(value[m_cursor]) & 0xC0) == 0x80)
                    {
                        --m_cursor;
                    }
                }
                return;
            case tui::Key::Right:
                if (m_cursor < value.size())
                {
                    ++m_cursor;
                    while (m_cursor < value.size() && (static_cast<unsigned char>(value[m_cursor]) & 0xC0) == 0x80)
                    {
                        ++m_cursor;
                    }
                }
                return;
            case tui::Key::Home:
                m_cursor = 0;
                return;
            case tui::Key::End:
                m_cursor = value.size();
                return;
            case tui::Key::Backspace:
                if (m_cursor > 0)
                {
                    const auto bytes = tui::Terminal::lastCodePointBytes(value.substr(0, m_cursor));
                    value.erase(m_cursor - bytes, bytes);
                    m_cursor -= bytes;
                }
                return;
            case tui::Key::Delete:
                if (m_cursor < value.size())
                {
                    const auto bytes = tui::Terminal::lastCodePointBytes(value.substr(0, m_cursor + 1));
                    value.erase(m_cursor, bytes);
                }
                return;
            case tui::Key::Character:
                value.insert(m_cursor, press.text);
                m_cursor += press.text.size();
                return;
            default:
                return;
        }
    }

    void activate()
    {
        auto& field = m_fields[m_current];
        if (field.kind == Kind::Text)
        {
            m_editing = true;
            m_cursor = field.value->size();
            m_editBackup = *field.value;
            return;
        }
        step(1);
    }

    /// Cycles a choice or a toggle. Text fields keep their value: arrows in edit mode move the
    /// cursor, and out of it they belong to whatever else the field can be.
    void step(const int direction)
    {
        auto& field = m_fields[m_current];
        if (field.kind == Kind::Toggle)
        {
            *field.flag = !*field.flag;
            return;
        }
        if (field.kind != Kind::Choice || field.choices.empty())
        {
            return;
        }
        const auto found = find(field.choices.begin(), field.choices.end(), *field.value);
        auto       index = found == field.choices.end() ? 0 : static_cast<size_t>(distance(field.choices.begin(), found));
        index = (index + field.choices.size() + static_cast<size_t>(direction < 0 ? field.choices.size() - 1 : 1)) % field.choices.size();
        *field.value = field.choices[index];
    }

    void commit()
    {
        m_editing = false;
        m_cursor = 0;
    }

    void save()
    {
        // The path is taken from the field first: validation has to see the file that would be
        // written, not the one the form started with.
        m_spec.file = m_runFilePath.empty() ? defaultRunFile(m_spec) : resolveRunPath(m_runFilePath);
        if (m_spec.out.empty())
        {
            m_spec.out = defaultOutDirectory(m_spec);
        }
        // Both are now part of what the file says, so show them as such if the person stays here.
        m_runFilePath = displayPath(m_spec.file);

        m_problems = validate(m_spec);
        if (!m_problems.empty())
        {
            return;
        }

        error_code error;
        if (!m_spec.file.parent_path().empty())
        {
            fs::create_directories(m_spec.file.parent_path(), error);
        }
        const auto text = toScript(m_spec);
        ofstream   output(m_spec.file, ios::binary | ios::trunc);
        output.write(text.data(), static_cast<streamsize>(text.size()));
        output.close();
        if (!output)
        {
            m_problems.push_back("cannot write " + m_spec.file.string() + ": " + string(strerror(errno)));
            return;
        }
        chmod(m_spec.file.c_str(), 0755);

        m_saved = true;
        m_done = true;
    }

    tui::Terminal  m_terminal;
    tui::Frame     m_frame;
    RunSpec&       m_spec;
    vector<Field>  m_fields;
    string         m_runFilePath;
    size_t         m_current {0};
    bool           m_editing {false};
    size_t         m_cursor {0};
    string         m_editBackup;
    vector<string> m_problems;
    bool           m_saved {false};
    bool           m_done {false};
};

} // namespace

FormAction editRunFile(RunSpec& spec)
{
    if (!tui::Terminal::available())
    {
        fprintf(stderr, "stand_runner --edit: needs a terminal; use --view for scripted output\n");
        return FormAction::Cancelled;
    }

    FormAction action = FormAction::Cancelled;
    {
        Form form(spec);
        action = form.run();
    }

    // The alternate screen is gone by now and the terminal is back to whatever was behind it: the
    // report belongs on a clean screen, not pasted over the shell's earlier output.
    printf("\033[2J\033[H");

    if (action == FormAction::Saved)
    {
        printf("saved: %s\n\n", spec.file.c_str());
        printf("run it:    %s\n", spec.file.c_str());
        printf("watch it:  stand_runner --view --run %s\n", spec.file.c_str());
    }
    return action;
}

} // namespace stand
