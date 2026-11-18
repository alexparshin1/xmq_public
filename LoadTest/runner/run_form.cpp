#include "run_form.h"

#include "tui.h"

#include <algorithm>
#include <cstdio>
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
            return *field.flag ? "yes" : "no";
        }
        const string value = *field.value;
        if (value.empty() && field.label == "out")
        {
            return "(stand_run.sh default)";
        }
        return value;
    }

    void draw()
    {
        const auto width = m_terminal.width();
        const auto height = m_terminal.height();

        m_terminal.clear();

        const auto title = m_spec.file.empty() ? "stand_runner --edit" : "stand_runner --edit  " + m_spec.file.string();
        m_terminal.moveTo(1, 1);
        m_terminal.write("\033[1m" + title + "\033[0m");

        int row = 3;
        for (size_t index = 0; index < m_fields.size(); ++index, ++row)
        {
            const auto& field = m_fields[index];
            const bool  selected = index == m_current;
            const bool  active = selected && m_editing;

            m_terminal.moveTo(row, 2);
            m_terminal.write(selected ? "\033[7m" : "");
            m_terminal.write(" " + field.label);
            for (auto padding = field.label.size() + 1; padding < 14; ++padding)
            {
                m_terminal.write(" ");
            }

            // The value is what the person changes, so it is what the highlight sits on; the help
            // text is dropped on a narrow terminal rather than pushing the value off the screen.
            string value = shownValue(field);
            if (field.kind != Kind::Text)
            {
                value = field.kind == Kind::Choice ? "< " + value + " >" : "[" + value + "]";
            }

            const auto valueWidth = static_cast<size_t>(max(10, width - 20 - static_cast<int>(field.help.size()) - 2));
            if (active && field.kind == Kind::Text)
            {
                // While editing, show the tail of a long value so the cursor stays visible.
                const auto  cursorColumns = tui::Terminal::displayWidth(value.substr(0, m_cursor));
                const auto  visible = min(valueWidth, static_cast<size_t>(max(4, width - 20)));
                const auto  start = static_cast<size_t>(max(0, cursorColumns - static_cast<int>(visible) + 1));
                const auto  offset = [&value, start]
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
                m_terminal.write("\033[7m " + value.substr(offset, visible) + "\033[0m");
            }
            else
            {
                m_terminal.write(selected ? "\033[7m " + value + "\033[0m" : " " + value);
            }

            if (!field.help.empty() && width > 60 && !selected)
            {
                const auto used = 20 + tui::Terminal::displayWidth(value) + 2;
                if (used < width)
                {
                    m_terminal.moveTo(row, used + 1);
                    m_terminal.write("\033[2m" + field.help.substr(0, static_cast<size_t>(width - used)) + "\033[0m");
                }
            }
            m_terminal.write(selected ? "\033[0m" : "");
        }

        row += 2;
        const auto& current = m_fields[m_current];
        m_terminal.moveTo(row, 2);
        m_terminal.write("\033[2m" + current.help.substr(0, static_cast<size_t>(max(0, width - 3))) + "\033[0m");

        // Problems sit between the help line and the keys, so they stay on screen on a short
        // terminal: a refusal the person cannot see is the same as no refusal.
        for (size_t index = 0; index < m_problems.size() && row + 2 + static_cast<int>(index) < height - 1; ++index)
        {
            m_terminal.moveTo(row + 2 + static_cast<int>(index), 2);
            m_terminal.write("\033[1m! " + m_problems[index] + "\033[0m");
        }

        m_terminal.moveTo(height - 1, 2);
        m_terminal.write(m_editing ? "editing: Enter keeps, Esc drops this field's edit"
                                   : "Enter to edit or switch, arrows to move, F2 saves");

        if (m_editing)
        {
            // Put the cursor on the character being typed; the rest of the screen is context.
            m_terminal.moveTo(3 + static_cast<int>(m_current), 17 + tui::Terminal::displayWidth(shownValue(m_fields[m_current]).substr(0, m_cursor)));
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
        m_problems = validate(m_spec);

        // The file the person typed is what gets written; empty means the default name.
        m_spec.file = m_runFilePath.empty() ? defaultRunFile(m_spec) : fs::path(m_runFilePath);
        if (m_spec.out.empty())
        {
            m_spec.out = defaultOutDirectory(m_spec);
        }
        // Both are now part of what the file says, so show them as such if the person stays here.
        m_runFilePath = m_spec.file.string();

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
            m_problems.push_back("cannot write " + m_spec.file.string());
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
