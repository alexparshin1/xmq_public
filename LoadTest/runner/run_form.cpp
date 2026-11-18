#include "run_form.h"

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

        m_fields.push_back({"version", "XMQ version under test; it names the bench profile to restart", Kind::Text, &m_spec.version, nullptr, {}});
        m_fields.push_back({"set", "Scenario set to run, from the *.txt files next to stand_run.sh", Kind::Choice, &m_spec.setFile, nullptr, sets});
        m_fields.push_back({"server", "Broker host under test", Kind::Text, &m_spec.server, nullptr, {}});
        m_fields.push_back({"nic", "Client NIC watched for dropped packets", Kind::Text, &m_spec.nic, nullptr, {}});
        m_fields.push_back({"subnet", "Client source subnet", Kind::Text, &m_spec.subnet, nullptr, {}});
        m_fields.push_back({"first", "First host octet in that subnet", Kind::Text, &m_spec.firstOctet, nullptr, {}});
        m_fields.push_back({"count", "Number of source addresses", Kind::Text, &m_spec.count, nullptr, {}});
        m_fields.push_back({"out", "Per-scenario logs; empty means results/raw/<stamp>-xmq<version>-bench", Kind::Text, &m_spec.out, nullptr, {}});
        m_fields.push_back({"run file", "Where to write this run file", Kind::Text, &m_runFilePath, nullptr, {}});
        m_fields.push_back({"file results", "File the record with file_results.py when the set succeeds", Kind::Toggle, nullptr, &m_spec.fileResults, yesNo});
        m_fields.push_back({"keep brokers", "Refuse to start if another broker owns the port, instead of stopping it", Kind::Toggle, nullptr, &m_spec.keepOtherBrokers, yesNo});
        m_fields.push_back({"skip preflight", "Do not run preflight.sh on either host", Kind::Toggle, nullptr, &m_spec.skipPreflight, yesNo});
        m_fields.push_back({"note", "Free-text line for the record", Kind::Text, &m_spec.note, nullptr, {}});

        m_runFilePath = m_spec.file.empty() ? defaultRunFile(m_spec).string() : m_spec.file.string();
    }

    [[nodiscard]] string shownValue(const Field& field) const
    {
        if (field.kind == Kind::Toggle)
        {
            return *field.flag ? "[X]" : "[ ]";
        }
        const string value = *field.value;
        if (value.empty() && field.label == "out")
        {
            return "(stand_run.sh default)";
        }
        return value;
    }

    /// Paints the screen the way a Turbo Vision application painted a dialog: a blue desktop with
    /// one grey box on it, a title in the frame, buttons along the bottom and the keys on a status
    /// line. Every colour comes from theme.h, so the look is one file's decision.
    void draw()
    {
        const auto width = m_terminal.width();
        const auto height = m_terminal.height();

        // Edge to edge: the dialog fills the screen, as the window in a Turbo Vision application did
        // once it was maximised. A centred box wastes the width the values need - the paths are
        // long - and the arrows are the only boundaries worth having.
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
        // The dialog ends on the line above the status bar, and its buttons sit just inside the
        // bottom edge, the way a Turbo Vision dialog put them.
        const auto bottom = max(helpRow + 4, height - 1);
        const auto buttonRow = bottom - 1;

        const auto put = [this](const int row, const int column, const string& text)
        {
            m_terminal.moveTo(row, column);
            m_terminal.write(text);
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

        // The desktop: every line, the full width, so the previous frame leaves nothing behind.
        for (int row = 1; row <= height; ++row)
        {
            put(row, 1, string(theme::desktop) + blank(static_cast<size_t>(width)) + theme::reset);
        }

        // The frame, with the file name in its title, and a shadow down the right and along the
        // bottom - the detail that made a Turbo Vision window look raised rather than painted on.
        const auto title = m_spec.file.empty() ? string {"stand_runner --edit"}
                                               : "stand_runner --edit  " + m_spec.file.string();
        for (int row = top + 1; row <= bottom; ++row)
        {
            put(row, left, string(theme::frame) + "│" + theme::reset);
            put(row, left + innerWidth + 1, string(theme::frame) + "│" + theme::reset);
            put(row, left + innerWidth + 2, string(theme::desktopDim) + " " + theme::reset);
        }
        put(bottom + 1, left + 2, string(theme::desktopDim) + blank(static_cast<size_t>(innerWidth + 1)) + theme::reset);

        // Single line: a dialog, not a window. Turbo Vision drew dialogs with one line and windows
        // with two, and the difference is what tells them apart at a glance.
        string topLine {"┌"};
        const string singleLine {"─"};
        for (int column = 0; column < innerWidth; ++column) topLine += singleLine;
        topLine += "┐";
        string bottomLine {"└"};
        for (int column = 0; column < innerWidth; ++column) bottomLine += singleLine;
        bottomLine += "┘";
        put(top, left, string(theme::frame) + topLine + theme::reset);
        put(bottom, left, string(theme::frame) + bottomLine + theme::reset);

        const auto titleText = " " + clip(title, innerWidth - 10) + " ";
        const auto titleColumn = left + 4 + max(0, (innerWidth - 6 - static_cast<int>(titleText.size())) / 2);
        put(top, titleColumn, string(theme::frameActive) + titleText + theme::reset);
        put(top, left + 1, string(theme::frameActive) + "[■]" + theme::reset);

        // One line per field: the label in the dialog's own colour, its shortcut letter standing
        // out, and the value in an input-line box.
        for (int index = 0; index < rows; ++index)
        {
            const auto& field = m_fields[static_cast<size_t>(index)];
            const bool  selected = static_cast<size_t>(index) == m_current;
            const bool  active = selected && m_editing;
            const int   row = contentTop + index;

            put(row, left + 1, string(theme::dialog) + blank(static_cast<size_t>(innerWidth)) + theme::reset);

            string label = field.label;
            put(row, contentLeft + 1, string(theme::dialog) + label.substr(0, 1) + theme::reset);
            label.erase(0, 1);
            put(row, contentLeft + 2, string(theme::dialog) + label + blank(static_cast<size_t>(max(0, labelWidth - 2 - static_cast<int>(label.size())))) + theme::reset);

            string value = shownValue(field);
            const bool placeholder = value.starts_with("(");
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
                put(row, valueLeft + 1, string(theme::selected) + " " + visible + blank(static_cast<size_t>(max(0, valueWidth - 1 - tui::Terminal::displayWidth(visible)))) + theme::reset);
            }
            else if (selected)
            {
                const auto text = clip(value, valueWidth - 1);
                put(row, valueLeft + 1, string(theme::selected) + " " + text + blank(static_cast<size_t>(max(0, valueWidth - 1 - tui::Terminal::displayWidth(text)))) + theme::reset);
            }
            else
            {
                // An input line on a grey dialog: white on blue, the palette entry cpInputLine reads as.
                const auto text = clip(value, valueWidth - 1);
                put(row, valueLeft + 1, string(theme::input) + " " + text + blank(static_cast<size_t>(max(0, valueWidth - 1 - tui::Terminal::displayWidth(text)))) + theme::reset);
            }
            (void) placeholder;
        }

        // The help for the field the keyboard is on, then anything that was refused.
        put(helpRow, contentLeft, string(theme::dialog) + " " + m_fields[m_current].help.substr(0, static_cast<size_t>(innerWidth - 2)) + blank(static_cast<size_t>(max(0, innerWidth - 2 - static_cast<int>(min<size_t>(m_fields[m_current].help.size(), static_cast<size_t>(innerWidth - 2)))))) + theme::reset);

        for (size_t index = 0; index < m_problems.size() && problemRow + static_cast<int>(index) < buttonRow; ++index)
        {
            const auto text = "! " + m_problems[index];
            put(problemRow + static_cast<int>(index), contentLeft, string(theme::warning) + text + blank(static_cast<size_t>(max(0, innerWidth - 2 - static_cast<int>(min<size_t>(text.size(), static_cast<size_t>(innerWidth - 2)))))) + theme::reset);
        }

        // Buttons along the bottom of the dialog, right-aligned as a dialog's were: the default one
        // green, the way out beside it in the dialog's own colour.
        {
            const string save {" F2 Save "};
            const string cancel {"[ Esc Cancel ]"};
            const auto   start = left + innerWidth - static_cast<int>(save.size() + cancel.size()) - 3;
            put(buttonRow, start, string(theme::button) + save + theme::reset);
            put(buttonRow, start + static_cast<int>(save.size()) + 2,
                string(theme::dialog) + "[" + theme::reset + string(theme::dialogDim) + " Esc" + theme::reset +
                    string(theme::dialog) + " Cancel ]" + theme::reset);
        }

        // Status line: what each key does, the key itself standing out.
        if (height >= bottom + 2)
        {
            // The same grey bar with red keys as the viewer: one screen's keys should look like the
            // other's, since they are read the same way.
            put(height, 1, string(theme::status) + blank(static_cast<size_t>(width)) + theme::reset);
            const auto line = string {"~↑↓~ move   ~Enter~ edit   ~F2~ save   ~Esc~ leave without saving"};
            int        column = 2;
            bool       hot = false;
            string     run;
            const auto flush = [&]
            {
                if (run.empty())
                {
                    return;
                }
                put(height, column, string(hot ? theme::statusKey : theme::status) + run + theme::reset);
                column += static_cast<int>(run.size());
                run.clear();
            };
            for (const auto character: line)
            {
                if (character == '~')
                {
                    flush();
                    hot = !hot;
                    continue;
                }
                run += character;
            }
            flush();
        }

        if (m_editing)
        {
            const auto prefix = shownValue(m_fields[m_current]).substr(0, m_cursor);
            put(contentTop + static_cast<int>(m_current), valueLeft + 2 + tui::Terminal::displayWidth(prefix), "");
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
        m_spec.file = m_runFilePath.empty() ? defaultRunFile(m_spec) : fs::path(m_runFilePath);
        if (m_spec.out.empty())
        {
            m_spec.out = defaultOutDirectory(m_spec);
        }
        // Both are now part of what the file says, so show them as such if the person stays here.
        m_runFilePath = m_spec.file.string();

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

    if (action == FormAction::Saved)
    {
        printf("saved: %s\n\n", spec.file.c_str());
        printf("run it:    %s\n", spec.file.c_str());
        printf("watch it:  stand_runner --view %s\n", spec.out.c_str());
        printf("with the broker's own CPU and memory:\n");
        printf("           stand_runner --view %s --broker %s\n", spec.out.c_str(), spec.server.c_str());
    }
    return action;
}

} // namespace stand
