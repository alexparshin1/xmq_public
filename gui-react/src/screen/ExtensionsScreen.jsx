import React from "react";
import {errorWindow, informationWindow} from "../components/MessageWindow";
import ControlAPI from "../ControlAPI";
import "./InputScreen.css";
import "./ExtensionsScreen.css";
import "../components/DialogWindow.css";
import PageTitle from "../components/PageTitle";
import HelpButton from "../components/HelpButton";
import PasswordInput from "../components/PasswordInput";
import WebhookRuleTest from "../components/WebhookRuleTest";
import {extensionForm, conditionMatches} from "../components/ExtensionForm";

/**
 * Extensions screen: every extension the broker has, and one page each.
 *
 * Every extension, not only the running ones - a switched-off extension is exactly the one somebody
 * comes here to look at, and a list that showed only what is running would answer the easy question
 * and hide the hard one.
 *
 * What each extension takes is declared by the extension itself, so this builds the form from what
 * the broker reports rather than from anything written here. An extension that declares nothing
 * still gets a page: its settings appear as plain text fields.
 */
export default class ExtensionsScreen extends React.Component {

    state = {
        extensions: [],
        selected: null,   // the name of the extension being looked at
        edited: {},       // settings the operator has changed but not applied
        preset: "",       // the starting point chosen from the extension's presets
        presetResult: null, // what pressing Preset filled, and what was already right
        running: true,    // whether the MQTT server is up; extensions live inside it
        busy: false,
        showAdd: false,   // the Add Data Integration dialog
        addType: "",
        addName: "",
        addError: ""
    }

    componentDidMount() {
        this.refresh();
    }

    /**
     * Reports a refused call and says whether the caller should carry on.
     *
     * asyncMakeAPICall answers with the parsed reply whether the server accepted the request or
     * refused it, so a screen that checks only for a missing reply treats a refusal as a silent
     * success - the list reloads, the typed value reverts, and nothing says why. That is exactly
     * what this page did.
     */
    static succeeded(data) {
        if (!data) {
            return false; // the call did not get through; asyncMakeAPICall has already said so
        }
        const error = ControlAPI.apiError(data);
        if (error) {
            errorWindow(error);
            return false;
        }
        return true;
    }

    /// Asks the broker for both facts this page needs: whether the server is up, and what it has.
    refresh = (message, selectAfter) => {
        ControlAPI.asyncMakeAPICall("ServerControl", {action: "status"})
            .then(status => {
                if (status) {
                    this.setState({running: !!status.running});
                }
                return ControlAPI.asyncMakeAPICall("ExtensionControl", {action: "list"});
            })
            .then(data => {
                if (!ExtensionsScreen.succeeded(data)) {
                    this.setState({busy: false});
                    return;
                }
                this.setState({extensions: data.list ? data.list : [], edited: {}, preset: "",
                       presetResult: null, busy: false,
                       selected: selectAfter || this.state.selected});
                if (message) {
                    // The title carries the verdict and the body carries what was done, because
                    // what the broker answers with is a list of lines - which file was written,
                    // which extension took its settings - and none of them says on its own that
                    // the whole thing succeeded. A refusal never reaches here: the server marks
                    // it as a failure and succeeded() shows it in the error window.
                    informationWindow(message, "Changes are accepted");
                }
            });
    }

    /// One settings set is named by its extension plus its ExtensionInstance; the default set has no
    /// extension instance, so its id is just the extension name and old configurations keep working.
    static extensionId(extension) {
        return extension.extension_instance ? extension.name + "/" + extension.extension_instance : extension.name;
    }

    selected() {
        return this.state.extensions.find(one => ExtensionsScreen.extensionId(one) === this.state.selected);
    }

    static humanName(extensionName) {
        return String(extensionName || "")
            .replace(/[_-]+/g, " ")
            .replace(/\b\w/g, letter => letter.toUpperCase())
            .trim();
    }

    /// One row per kind of data integration the broker can create; the description is the
    /// extension's own, so adding a kind needs no change here.
    addTypes() {
        const seen = new Set();
        return this.state.extensions.filter(extension => {
            if (extension.sink || seen.has(extension.name)) return false;
            seen.add(extension.name);
            return true;
        });
    }

    openAdd = () => {
        const types = this.addTypes();
        if (!types.length) return;
        const first = types[0].name;
        let name = ExtensionsScreen.humanName(first);
        let suffix = 2;
        while (this.state.extensions.some(one => (one.display_name || "").toLowerCase() === name.toLowerCase())) {
            name = ExtensionsScreen.humanName(first) + " " + suffix++;
        }
        this.setState({showAdd: true, addType: first, addName: name, addError: ""});
    }

    addDescription() {
        const type = this.addTypes().find(extension => extension.name === this.state.addType);
        return type ? (type.description || "No description supplied by this extension.") : "";
    }

    submitAdd = () => {
        const name = this.state.addName.trim();
        if (!name) {
            this.setState({addError: "Enter a name for the data integration."});
            return;
        }
        if (this.state.extensions.some(one => (one.display_name || "").toLowerCase() === name.toLowerCase())) {
            this.setState({addError: "A data integration with this name already exists."});
            return;
        }
        const type = this.state.addType;
        let instance = type.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || type;
        let suffix = 2;
        while (this.state.extensions.some(one => one.name === type && one.extension_instance === instance)) {
            instance = type + "-" + suffix++;
        }
        this.setState({busy: true, showAdd: false});
        ControlAPI.asyncMakeAPICall("ExtensionControl",
            {action: "add_instance", name: type, extension_instance: instance, display_name: name})
            .then(data => {
                this.setState({busy: false});
                if (!ExtensionsScreen.succeeded(data)) return;
                this.refresh(data.message, type + "/" + instance);
            });
    }

    toggleExtension = (extension) => {
        this.setState({busy: true});
        ControlAPI.asyncMakeAPICall("ExtensionControl", {
            action: extension.extension_enabled ? "disable_extension" : "enable_extension",
            name: extension.name
        }).then(data => {
            this.setState({busy: false});
            if (!ExtensionsScreen.succeeded(data)) return;
            this.refresh(data.message);
        });
    }

    renameIntegration = (extension) => {
        const current = extension.display_name || "";
        const name = window.prompt("Rename data integration:", current);
        if (!name || !name.trim() || name.trim() === current) return;
        this.setState({busy: true});
        ControlAPI.asyncMakeAPICall("ExtensionControl",
            {action: "rename_instance", name: extension.name, extension_instance: extension.extension_instance,
             display_name: name.trim()})
            .then(data => {
                this.setState({busy: false});
                if (!ExtensionsScreen.succeeded(data)) return;
                this.refresh(data.message, ExtensionsScreen.extensionId(extension));
            });
    }

    deleteIntegration = (extension) => {
        const label = extension.display_name || extension.extension_instance;
        if (!window.confirm("Delete the data integration '" + label + "' and its persistent queue? This cannot be undone.")) return;
        this.setState({busy: true});
        ControlAPI.asyncMakeAPICall("ExtensionControl",
            {action: "remove_instance", name: extension.name, extension_instance: extension.extension_instance})
            .then(data => {
                this.setState({busy: false});
                if (!ExtensionsScreen.succeeded(data)) return;
                this.refresh(data.message);
            });
    }

    renderAddDialog() {
        if (!this.state.showAdd) return null;
        return <div className="modalBackground">
            <div className="modalContainer dataIntegrationDialog" role="dialog" aria-label="Add Data Integration">
                <div className="title">Add Data Integration</div>
                <div className="body">
                    <label htmlFor="dataIntegrationType">Type:</label>
                    <select id="dataIntegrationType" value={this.state.addType}
                            onChange={event => {
                                const type = event.target.value;
                                let name = ExtensionsScreen.humanName(type);
                                let suffix = 2;
                                while (this.state.extensions.some(one =>
                                    (one.display_name || "").toLowerCase() === name.toLowerCase())) {
                                    name = ExtensionsScreen.humanName(type) + " " + suffix++;
                                }
                                this.setState({addType: type, addName: name, addError: ""});
                            }}>
                        {this.addTypes().map(extension =>
                            <option key={extension.name} value={extension.name}>
                                {ExtensionsScreen.humanName(extension.name)}
                            </option>)}
                    </select>
                    <p className="extensionNote">{this.addDescription()}</p>
                    <label htmlFor="dataIntegrationName">Name:</label>
                    <input id="dataIntegrationName" type="text" value={this.state.addName}
                           onChange={event => this.setState({addName: event.target.value, addError: ""})}/>
                    {this.state.addError ? <p role="alert" className="extensionNote extensionValueError">
                        {this.state.addError}
                    </p> : null}
                </div>
                <div className="footer">
                    <button className="modalCancelButton" onClick={() => this.setState({showAdd: false})}>Cancel</button>
                    <button className="modalOkButton" disabled={this.state.busy} onClick={this.submitAdd}>Add</button>
                </div>
            </div>
        </div>;
    }

    /// What a setting holds right now: what the operator typed, or what the broker reported.
    valueOf(setting) {
        const edited = this.state.edited[setting.name];
        if (edited !== undefined) {
            return edited;
        }
        const value = setting.value === undefined ? "" : setting.value;
        if (setting.name === "db_type" && setting.default_value === "Use saved URI" &&
            (value === "" || value === "Use saved URI") &&
            !(this.selected()?.settings || []).some(field => field.name === "database_uri" && field.value)) {
            return "postgresql";
        }
        if (value !== "") {
            return value;
        }
        const extension = this.selected();
        if (extension?.name === "webhook") {
            const find = name => (extension.settings || []).find(field => field.name === name);
            const stored = name => find(name)?.value;
            const sources = stored("sources") || "Send messages";
            const template = find("template");
            const legacy = {
                mqtt_messages: sources !== "Send log" ? "true" : "false",
                log_messages: sources !== "Send messages" ? "true" : "false",
                mqtt_filter: stored("sql_rule") ? "SQL rules" : "Topics",
                mqtt_template: template?.value || template?.default_value || "",
                log_template: template?.value || template?.default_value || ""
            };
            if (Object.hasOwn(legacy, setting.name)) return legacy[setting.name];
        }
        // An unset setting starts at the value the extension would use anyway, rather than as an
        // empty field that Apply would write back as an empty string.
        return setting.default_value === undefined ? "" : setting.default_value;
    }

    hasChanges() {
        return Object.keys(this.state.edited).length > 0;
    }

    onSettingChange = (name, value) => {
        this.setState(state => ({edited: {...state.edited, [name]: value}}));
    }

    /// The items of the list setting, as they are shown now: what was typed, else what is stored.
    listOf(setting) {
        return ExtensionsScreen.listItems(this.valueOf(setting));
    }

    changeListItem = (setting, index, value) => {
        const items = this.listOf(setting);
        items[index] = value;
        this.onSettingChange(setting.name, JSON.stringify(items));
    }

    addListItem = (setting) => {
        const items = this.listOf(setting);
        items.push("");
        this.onSettingChange(setting.name, JSON.stringify(items));
    }

    removeListItem = (setting, index) => {
        const items = this.listOf(setting);
        items.splice(index, 1);
        this.onSettingChange(setting.name, JSON.stringify(items));
    }

    /**
     * Whether a value has the shape the extension declared, if it declared one.
     *
     * The pattern is anchored here as well as in the broker: a setting describes a whole value,
     * not a fragment of one, and "digits only" must not accept "12abc". An empty value is left to
     * the setting's default, and a pattern this browser cannot compile is the broker's to judge -
     * the broker checks every value again, and refuses the ones that do not match.
     */
    static matchesPattern(pattern, value) {
        if (!pattern || value === undefined || value === null || value === "") {
            return true;
        }
        try {
            return new RegExp("^(?:" + pattern + ")$").test(value);
        } catch (error) {
            return true;
        }
    }

    /// The items of a list setting. A bare value is one item, so a setting written by hand - or
    /// one that has never been edited here - still shows as a list of one.
    static listItems(value) {
        const text = value === undefined || value === null ? "" : String(value).trim();
        if (!text) {
            return [];
        }
        if (text.startsWith("[")) {
            try {
                const parsed = JSON.parse(text);
                if (Array.isArray(parsed)) {
                    return parsed.map(item => String(item));
                }
            } catch (error) {
                // Not JSON after all: kept as one item so it can be seen and repaired, rather
                // than disappearing from the form.
            }
        }
        return [text];
    }

    static matches(setting, value) {
        if (setting.type === "list") {
            // An empty list is how the form says "not set": the extension's default applies.
            return ExtensionsScreen.listItems(value)
                .every(item => item !== "" && ExtensionsScreen.matchesPattern(setting.pattern, item));
        }
        return ExtensionsScreen.matchesPattern(setting.pattern, value);
    }

    invalidSettings() {
        const extension = this.selected();
        return (extension && extension.settings ? extension.settings : [])
            .filter(setting => !ExtensionsScreen.matches(setting, this.valueOf(setting)));
    }

    /**
     * Parse a rule draft before showing the test form or saving it. The broker uses the same
     * parser in both places; this call is a preflight and sends no HTTP request.
     */
    validateSqlSetting = async setting => {
        const test = {
            source: setting.name === "log_sql_rule" ? "logs" : "mqtt",
            sql: this.valueOf(setting), topic: "test/topic", payload: "", message: "",
            client_id: "", username: "", qos: "1", retain: "false",
            level: "error", subject: "server_events", timestamp: new Date().toISOString()
        };
        try {
            const response = await ControlAPI.asyncMakeAPICall("ExtensionControl", {
                action: "test_sql",
                settings: Object.entries(test).map(([name, value]) => ({name, value}))
            }, true);
            const problem = response ? ControlAPI.apiError(response) : "No response from the broker.";
            if (problem) {
                errorWindow(`Invalid ${setting.label || setting.name}: ${problem}`, "Cannot test or save the rule");
                return false;
            }
        } catch {
            errorWindow(`Could not validate ${setting.label || setting.name}.`, "Cannot test or save the rule");
            return false;
        }
        return true;
    };

    openRuleTest = async (setting, sql, logs) => {
        if (!await this.validateSqlSetting(setting)) return;
        this.setState({ruleTest: {sql, logs}});
    };

    apply = (afterSet) => {
        const extension = this.selected();
        if (!extension) {
            return;
        }

        // Checked again even though Apply is disabled while something is wrong: the value can be
        // pasted in and the click can land before React has rendered the disabled button.
        if (this.invalidSettings().length) {
            errorWindow("Some settings do not have the right format. See the marked fields.",
                        "Changes are not applied");
            return;
        }

        // Every setting is sent, not only the changed ones: the server replaces the block wholesale
        // so that a key removed here is removed there, and sending a partial list would make
        // removal impossible.
        const settings = (extension.settings || []).map(setting => ({
            name: setting.name,
            value: this.valueOf(setting)
        }));

        // SQL is not a pattern the extension can declare. The broker checks the draft before it
        // writes settings; this screen only supplies the reason for a refusal.
        const sqlSettings = (extension.settings || [])
            .filter(setting => ["sql_rule", "log_sql_rule"].includes(setting.name) &&
                               this.valueOf(setting).trim());
        const validateSql = async () => {
            for (const setting of sqlSettings) {
                if (!await this.validateSqlSetting(setting)) return false;
            }
            return true;
        };

        this.setState({busy: true});
        validateSql().then(valid => {
            if (!valid) {
                this.setState({busy: false});
                return;
            }
            ControlAPI.asyncMakeAPICall("ExtensionControl",
                                        {action: "set", name: extension.name,
                                         extension_instance: extension.extension_instance || "", settings: settings})
                .then(data => {
                    this.setState({busy: false});
                    if (!ExtensionsScreen.succeeded(data)) {
                        this.restore(data);
                        return;
                    }
                    if (typeof afterSet === "function")
                    {
                        afterSet().then(() => this.refresh(data.message));
                    }
                    else
                    {
                        this.refresh(data.message);
                    }
                });
        });
    }

    /**
     * Puts the form back to what the broker is actually using.
     *
     * A refused value left in the field says it was taken. The broker sends the list even when it
     * refuses - what it sends is the state the refusal preserved - so the form goes back to that
     * rather than to what was typed, and the operator can see the difference between what they
     * asked for and what is in force.
     */
    restore = (data) => {
        if (data && data.list) {
            this.setState({extensions: data.list, edited: {}});
        }
    }

    switchTo = (on) => {
        const extension = this.selected();
        if (!extension) {
            return;
        }

        this.setState({busy: true});
        ControlAPI.asyncMakeAPICall("ExtensionControl",
                                    {action: on ? "enable" : "disable", name: extension.name,
                                     extension_instance: extension.extension_instance || ""})
            .then(data => {
                this.setState({busy: false});
                if (!ExtensionsScreen.succeeded(data)) {
                    this.restore(data);
                    return;
                }
                this.refresh(data.message);
            });
    }

    capabilitiesOf(extension) {
        const declared = [];
        if (extension.observer) {
            declared.push("observer");
        }
        if (extension.authenticator) {
            declared.push("authenticator");
        }
        if (extension.authorizer) {
            declared.push("authorizer");
        }
        if (extension.sink) {
            declared.push("sink");
        }
        return declared.length ? declared.join(", ") : "none declared";
    }

    renderList() {
        if (!this.state.extensions.length) {
            return <div className="inputScreenIntro">
                <p>
                    {this.state.running
                        ? "This broker has no extensions configured. They are listed in " +
                          "xmq_extensions.conf, or one file each in xmq_extensions.d beside it."
                        : "The MQTT server is stopped, and extensions live inside it - so there " +
                          "is nothing loaded to show. Start the server on the Dashboard."}
                </p>
            </div>;
        }

        return <div>
            <div className="extensionListButtons">
                <button className="modalOkButton" disabled={this.state.busy || !this.addTypes().length}
                        onClick={this.openAdd}>Add Data Integration</button>
            </div>
            <table className="extensionList">
            <thead>
            <tr>
                <th>Data Integration</th>
                <th>Version</th>
                <th>State</th>
                <th>Role</th>
                <th>What it is for</th>
                <th>Actions</th>
            </tr>
            </thead>
            <tbody>
            {[...new Set(this.state.extensions.filter(extension => !extension.sink).map(extension => extension.name))]
                .map(name => {
                    const sets = this.state.extensions.filter(extension => !extension.sink && extension.name === name);
                    const canAdd = name === "message_export" || name === "log_export";
                    return <React.Fragment key={name}>
                        <tr className="extensionGroupRow"
                            onClick={() => sets.length && this.setState({selected: ExtensionsScreen.extensionId(sets[0]),
                                          edited: {}, preset: "", presetResult: null})}>
                            <td colSpan="6">
                                <b>{name}</b>
                            </td>
                        </tr>
                        {sets.map(extension => {
                            const id = ExtensionsScreen.extensionId(extension);
                            return <tr key={id}
                                className={id === this.state.selected ? "extensionRowSelected" : ""}
                                onClick={() => this.setState({selected: id, edited: {}, preset: "",
                                              presetResult: null})}>
                                <td>{extension.display_name || extension.extension_instance || "default"}</td>
                                <td>{extension.version || "—"}</td>
                                <td className={extension.running ? "extensionRunning" : "extensionStopped"}>
                                    {extension.running ? "running" : "stopped"}
                                    {extension.required ? " (required)" : ""}
                                </td>
                                <td>{this.capabilitiesOf(extension)}</td>
                                <td>{extension.description || "—"}</td>
                                <td>
                                    {extension.extension_instance
                                        ? <React.Fragment>
                                            <button className="modalCancelButton"
                                                    disabled={this.state.busy}
                                                    onClick={(event) => { event.stopPropagation(); this.renameIntegration(extension); }}>
                                                Rename
                                            </button>
                                            <button className="modalCancelButton"
                                                    disabled={this.state.busy}
                                                    onClick={(event) => { event.stopPropagation(); this.deleteIntegration(extension); }}>
                                                Delete
                                            </button>
                                          </React.Fragment>
                                        : null}
                                </td>
                            </tr>;
                        })}
                    </React.Fragment>;
                })}
            </tbody>
            </table>
            {this.renderAddDialog()}
        </div>;
    }

    /// One row of the form: a label, a value, and the [?] that explains it.
    renderRow(label, value, help) {
        return <div className="extensionRow" key={label}>
            <label>{label}:</label>
            <div className="extensionValue">{value}</div>
            <div className="extensionHelp">
                {help ? <HelpButton title={label} text={help}/> : null}
            </div>
        </div>;
    }

    renderDashboard(extension) {
        // Counters worth a line only for the capability that produces them: an observer's event
        // count says nothing about an authenticator, and a row of zeroes reads as a fault.
        const rows = [
            ["Version", extension.version || "\u2014", null],
            ["Role", this.capabilitiesOf(extension), null],
            ["State", extension.running ? "running" : "stopped", null],
            ["Library", <code>{extension.library}</code>, null],
            ["Configured in", <code>{extension.source}</code>,
             "The file this extension's entry was read from. Edit it there, or use the fields below."]
        ];

        if (extension.observer) {
            rows.push(["Events delivered", extension.events_delivered, null]);
        }
        if (extension.authenticator) {
            rows.push(["Connections admitted", extension.admitted, null]);
            rows.push(["Connections refused", extension.refused, null]);
            rows.push(["Store unreachable", extension.store_errors,
                       "Times this extension answered that it could not reach what it authenticates " +
                       "against. Those connections were refused rather than admitted or passed on."]);
        }
        if (extension.message_tap) {
            rows.push(["Messages delivered", extension.messages_delivered, null]);
            rows.push(["Messages dropped", extension.messages_dropped,
                       "Messages the tap could not take because its queue was full, or because the " +
                       "body was larger than the maximum the extension asked for. Nothing waits " +
                       "for a tap, so a receiver that falls behind costs messages, not throughput."]);
        }
        if (extension.log_tap) {
            rows.push(["Log records delivered", extension.logs_delivered, null]);
            rows.push(["Log records dropped", extension.logs_dropped,
                       "Log records the tap could not take because its queue was full. The two " +
                       "taps have separate queues, so a slow log receiver does not delay messages."]);
        }

        if (["message_export", "log_export"].includes(extension.name) &&
            extension.schema_rejected != null) {
            rows.push(["Schema rejections", extension.schema_rejected, null],
                      ["Sink failures", extension.sink_failures, "Failed delivery attempts, including retries."],
                      ["SQLite queue failures", extension.queue_failures, "SQLite queue operations that failed, including when the queue limit was reached."],
                      ["Dead-letter publishes accepted", extension.dead_letter_accepted,
                       "The broker accepted these publishes for asynchronous delivery."],
                      ["Dead-letter publish failures", extension.dead_letter_failures,
                       "Dead-letter publish attempts the broker refused."]);
            if (extension.last_schema_keyword) {
                rows.push(["Last schema rejection",
                           <code>{extension.last_schema_path || "(root)"} · {extension.last_schema_keyword}</code>,
                           "JSON Pointer and validation keyword for the most recent rejection."]);
            }
        }

        return <div className="extensionForm">
            {rows.map(([label, value, help]) => this.renderRow(label, value, help))}
        </div>;
    }

    /**
     * The extension's starting points, as one control above the form.
     *
     * A preset is the extension saying which of its settings belong together - a directory type and
     * the matching filter, for instance. Choosing one only fills the fields: nothing is saved and
     * nothing reaches the broker until Apply, so the operator still sees exactly what will be
     * stored. Names the extension does not declare are ignored, and a preset can only set what the
     * form already shows.
     */
    renderPresets(extension) {
        const presets = this.presetsOf(extension);
        const chosen = presets.find(one => one.name === this.state.preset);
        const result = this.state.presetResult;
        return <div className="extensionPreset">
            <label htmlFor="extensionPresetChoice">Set defaults</label>
            <select id="extensionPresetChoice" value={this.state.preset || ""}
                    onChange={event => this.setState({preset: event.target.value, presetResult: null})}>
                <option value="">Choose a starting point</option>
                {presets.map(preset =>
                    <option key={preset.name} value={preset.name}>{preset.label || preset.name}</option>)}
            </select>
            <button type="button" className="extensionListButton" disabled={!chosen}
                    onClick={this.applyPreset}>Preset</button>
            {chosen
                ? <div className="extensionPresetHelp">
                    {chosen.description}
                    {/* Says what the button just did. A preset that only restates values the form
                        already holds looks like a button that does nothing, and "filled nothing"
                        is the wrong conclusion to draw from it. */}
                    {result
                        ? <span className="extensionPresetResult">
                            {result.filled.length
                                ? "Filled: " + result.filled.join(", ") + ". "
                                : "Every setting it names already had this value. "}
                            {result.unchanged.length
                                ? "Already at these values: " + result.unchanged.join(", ") + ". "
                                : ""}
                            Nothing is saved until Apply.
                          </span>
                        : <span className="extensionPresetNote">
                            Fill the fields below, check them, then Apply.
                          </span>}
                  </div>
                : <div className="extensionPresetHelp">
                    A starting point fills the settings that belong together; nothing is saved
                    until Apply.
                  </div>}
        </div>;
    }

    /// Fills the form from the chosen preset. Only declared settings are touched, so a preset
    /// written against a newer extension cannot add a key the form never shows. What changed and
    /// what was already right are both reported: the buttons that change nothing are the ones an
    /// operator needs to understand.
    applyPreset = () => {
        const extension = this.selected();
        const preset = extension && this.presetsOf(extension)
            .find(one => one.name === this.state.preset);
        if (preset) {
            this.fillFromPreset(preset, {}, false);
        }
    }

    fillFromPreset = (preset, alsoEdited, selectPreset) => {
        const extension = this.selected();
        const declared = new Map((extension.settings || [])
            .map(setting => [setting.name, setting]));
        const edited = {...this.state.edited, ...alsoEdited};
        const filled = [];
        const unchanged = [];
        (preset.settings || []).forEach(value => {
            const setting = declared.get(value.name);
            if (!setting || setting.type === "secret" || setting.declared === false) {
                return;
            }
            const wanted = value.value === undefined ? "" : String(value.value);
            const label = setting.label || setting.name;
            if (this.valueOf(setting) === wanted) {
                unchanged.push(label);
                return;
            }
            edited[value.name] = wanted;
            filled.push(label);
        });
        this.setState(state => ({
            edited,
            presetResult: {filled, unchanged},
            preset: selectPreset ? preset.name : state.preset
        }));
    }

    /**
     * A choice that names a profile brings the settings that belong with it.
     *
     * A directory type decides the filter, the group matching and the account check; leaving them
     * on "auto" hides that, and the form then looks identical for two modes that behave
     * differently. Applying the linked preset makes the difference visible where the choice is
     * made, and the operator still applies the result.
     */
    onChoiceChange = (setting, value) => {
        const extension = this.selected();
        const profile = (extension ? this.presetsOf(extension) : []).find(one =>
            one.choice_setting === setting.name && one.choice_value === value);
        if (!profile) {
            this.onSettingChange(setting.name, value);
            return;
        }
        this.fillFromPreset(profile, {[setting.name]: value}, true);
    }

    presetsOf(extension) {
        const {form} = extensionForm(extension);
        return form && form.presets !== undefined ? form.presets : extension.presets || [];
    }

    renderFields(extension) {
        const {form, error} = extensionForm(extension);
        const settings = extension.settings || [];
        const byName = new Map(settings.map(setting => [setting.name, setting]));
        const valueOf = name => this.valueOf(byName.get(name));
        const listed = new Set();
        const splitHttpUrl = ["message_export", "log_export"].includes(extension.name) &&
            byName.has("url") && byName.has("allow_http");
        if (form && splitHttpUrl) listed.add("allow_http");
        const sections = form ? form.sections.map((section, index) => {
            if (section.toggle) listed.add(section.toggle);
            const visible = conditionMatches(section.visible_when, valueOf);
            const open = visible && (!section.toggle || valueOf(section.toggle) === "true");
            const fields = section.fields.map(field => {
                listed.add(field.name);
                if (splitHttpUrl && field.name === "allow_http") return null;
                return open && conditionMatches(field.visible_when, valueOf)
                    ? this.renderSetting(byName.get(field.name), field,
                                         !conditionMatches(field.enabled_when, valueOf)) : null;
            }).filter(Boolean);
            return visible && (section.toggle || fields.length) ? <section className="extensionFormSection" key={index}>
                {section.toggle ? this.renderSetting(byName.get(section.toggle)) : <h3>{section.title}</h3>}
                {open && section.description ? <p className="extensionNote">{section.description}</p> : null}
                {fields}
            </section> : null;
        }) : [];
        return <div className="extensionForm">
            {error ? <p role="alert" className="extensionValueError">{error}</p> : null}
            {sections}
            {settings.filter(setting => !listed.has(setting.name) &&
                !(form && extension.name === "webhook" && ["sources", "template"].includes(setting.name)))
                .map(setting => this.renderSetting(setting))}
        </div>;
    }

    renderSetting(setting, presentation = {}, disabled = false) {
        const value = this.valueOf(setting);
        const change = event => this.onSettingChange(setting.name, event.target.value);
        const exporter = this.selected();
        const isExporter = ["message_export", "log_export"].includes(exporter?.name);
        const isLogSource = ["webhook", "log_export"].includes(exporter?.name);
        const isMqttSource = ["webhook", "message_export"].includes(exporter?.name);
        const webhookHeaders = setting.name === "headers" &&
            (isExporter || exporter?.name === "webhook");
        const mqttSqlHelp = setting.name === "sql_rule" && isMqttSource;
        const sinkNames = setting.name === "sink"
            ? (this.state.extensions || []).filter(extension => extension.sink).map(extension => extension.name)
            : [];

        let control;
        if (setting.name === "url" && isExporter &&
            exporter.settings.some(field => field.name === "allow_http")) {
            const prefix = value.startsWith("http://") ||
                (!value && this.valueOf(exporter.settings.find(field => field.name === "allow_http")) === "true")
                ? "http://" : "https://";
            const address = value.replace(/^https?:\/\//, "");
            const changeUrl = (protocol, host, explicitProtocol = false) => {
                const pasted = host.match(/^(https?:\/\/)(.*)$/i);
                const scheme = pasted ? pasted[1].toLowerCase() : protocol;
                const target = pasted ? pasted[2] : host;
                this.setState(({edited}) => ({edited: {...edited,
                    url: target ? scheme + target : "",
                    ...(explicitProtocol || pasted || !value ? {allow_http: String(scheme === "http://")} : {})
                }}));
            };
            control = <div className="extensionHttpUrl">
                <select aria-label="URL protocol" value={prefix}
                        onChange={event => changeUrl(event.target.value, address, true)}>
                    <option>https://</option>
                    <option>http://</option>
                </select>
                <input aria-label="URL address" type="text" value={address}
                       placeholder="example.com/hook"
                       onChange={event => changeUrl(prefix, event.target.value)}/>
            </div>;
        } else if (sinkNames.length) {
            control = <select value={value} onChange={change}>
                {value && !sinkNames.includes(value) ? <option key={value} value={value}>{value}</option> : null}
                {sinkNames.map(name => <option key={name} value={name}>{name}</option>)}
            </select>;
        } else
        if (setting.type === "choice") {
            control = <select value={value}
                              onChange={event => this.onChoiceChange(setting, event.target.value)}>
                {/* The stored value first, even when the extension no longer offers it: a value
                    silently replaced by the first of the list is a setting changed without asking. */}
                {value && !(setting.choices || "").split(",").map(one => one.trim()).includes(value)
                    ? <option key={value} value={value}>{value}</option>
                    : null}
                {(setting.choices || "").split(",").map(choice => choice.trim()).filter(choice => choice &&
                    (setting.name !== "db_type" || choice !== "Use saved URI" ||
                        (this.selected()?.settings || []).some(field => field.name === "database_uri" && field.value)))
                    .map(choice => <option key={choice} value={choice}>{choice}</option>)}
            </select>;
        } else if (setting.type === "boolean") {
            control = <input type="checkbox" checked={value === "true"}
                             onChange={event => this.onSettingChange(setting.name,
                                                                     event.target.checked ? "true" : "false")}/>;
        } else if (setting.type === "list") {
            const items = this.listOf(setting);
            control = <div className="extensionListEditor">
                {items.map((item, index) =>
                    <div className="extensionListRow" key={index}>
                        <input type="text" value={item}
                               pattern={setting.pattern || undefined}
                               title={setting.pattern ? "Must match " + setting.pattern : undefined}
                               onChange={event => this.changeListItem(setting, index, event.target.value)}/>
                        <button type="button" className="extensionListButton"
                                onClick={() => this.removeListItem(setting, index)}>Remove</button>
                    </div>)}
                <div className="extensionListFooter">
                    <button type="button" className="extensionListButton"
                            onClick={() => this.addListItem(setting)}>Add</button>
                    {items.length
                        ? null
                        : <span className="extensionListEmpty">
                            {webhookHeaders ? "No custom headers." : "No items; the extension's default applies."}
                          </span>}
                </div>
            </div>;
        } else if (setting.type === "integer") {
            // Text rather than number when a pattern is declared: a number field silently accepts
            // "1e5" and "-3", and refuses to say what is wrong with them.
            control = setting.pattern
                ? <input type="text" inputMode="numeric" pattern={setting.pattern} value={value}
                         onChange={change} title={"Must match " + setting.pattern}/>
                : <input type="number" value={value} onChange={change}/>;
        } else if (setting.type === "secret") {
            // The broker never sends a secret, only whether one is set. Leaving the mask alone
            // leaves the stored value alone; typing over it replaces it.
            control = <PasswordInput value={value} onChange={change} placeholder="not set"/>;
        } else if (presentation.widget === "multiline") {
            control = <textarea rows={4} value={value} onChange={change}/>;
        } else {
            control = <input type="text" value={value} onChange={change}
                             pattern={setting.pattern || undefined}
                             title={setting.pattern ? "Must match " + setting.pattern : undefined}/>;
        }

        // An undeclared key is worth explaining rather than hiding: a misspelling is the commonest
        // reason a setting quietly does nothing.
        const matches = ExtensionsScreen.matches(setting, value);
        // Each part is a sentence: the descriptions are written without a trailing full stop, so
        // one is added here rather than in every extension's declaration. A choice always has one
        // of its options selected, so "Default:" would suggest an unset state that does not exist.
        const sentence = (text) =>
        {
            const trimmed = (text || "").trim();
            if (!trimmed) {
                return "";
            }
            return /[.!?]$/.test(trimmed) ? trimmed + " " : trimmed + ". ";
        };
        const help = setting.declared === false
            ? "Set here, but this extension does not declare a setting of that name - usually a " +
              "misspelling, and then it has no effect."
            : (sentence(setting.description) +
               (setting.type !== "choice" && setting.default_value
                   ? "Default: " + setting.default_value + ". " : "") +
               (setting.pattern
                   ? (setting.type === "list" ? "Each item must match " : "Format: must match ") +
                     setting.pattern + ". " : "") +
               (setting.type === "list" && !webhookHeaders ? "An empty list leaves the setting at its default." : ""))
                  .trim();

        return <div className="extensionRow" key={setting.name}>
            <label>
                {(setting.label || setting.name)}
                {setting.required ? <b className="extensionRequired"> *</b> : null}:
            </label>
            <div className={matches ? "extensionValue" : "extensionValue extensionValueInvalid"}>
                <fieldset className="extensionFieldControl" disabled={disabled || this.selected()?.sink}>{control}
                    {(mqttSqlHelp || (setting.name === "log_sql_rule" &&
                        isLogSource)) &&
                        <button type="button" disabled={!value.trim()}
                                onClick={() => this.openRuleTest(setting, value, !mqttSqlHelp)}>
                            Test rule
                        </button>}
                </fieldset>
                {this.state.ruleTest?.sql === value &&
                    this.state.ruleTest.logs === (setting.name === "log_sql_rule") &&
                    (mqttSqlHelp || setting.name === "log_sql_rule") &&
                    <WebhookRuleTest {...this.state.ruleTest}
                                     onClose={() => this.setState({ruleTest: null})}/>}
                {matches ? null
                    : <div className="extensionValueError">
                        {/sql_rule:/.test(help) ? help : "This value does not match the format: " + setting.pattern}
                      </div>}
            </div>
            <div className="extensionHelp">
                {help ? <HelpButton title={setting.label || setting.name} text={help}
                    details={setting.name === "json_schema" && isExporter ? <div>
                        <p>Use the field names produced by your SQL rule or template.</p>
                        <pre style={{whiteSpace: "pre-wrap"}}>{JSON.stringify({
                            type: "object",
                            required: [isLogSource ? "level" : "temperature"],
                            properties: isLogSource
                                ? {level: {type: "string"}}
                                : {temperature: {type: "number"}}
                        }, null, 2)}</pre>
                        <p><a href="https://json-schema.org/learn/getting-started-step-by-step"
                              target="_blank" rel="noopener noreferrer">
                            JSON Schema documentation (opens in a new tab)
                        </a></p>
                    </div> : mqttSqlHelp ? <div>
                        <table className="mqttSqlHelpFields">
                            <thead><tr><th>SELECT field</th><th>Value</th></tr></thead>
                            <tbody>
                                <tr><td><code>topic</code></td><td>MQTT topic name</td></tr>
                                <tr><td><code>client_id</code></td><td>Publisher client ID</td></tr>
                                <tr><td><code>username</code></td><td>Publisher username</td></tr>
                                <tr><td><code>timestamp</code></td><td>Webhook timestamp (ISO 8601)</td></tr>
                                <tr><td><code>qos</code></td><td>MQTT QoS (number)</td></tr>
                                <tr><td><code>retain</code></td><td>MQTT retain flag (boolean)</td></tr>
                                <tr><td><code>payload</code></td><td>Whole message body as UTF-8 text; null for invalid UTF-8</td></tr>
                                <tr><td><code>payload.field</code></td><td>Scalar JSON value; nested paths work too</td></tr>
                            </tbody>
                        </table>
                        <p><code>FROM</code> takes one or more comma-separated quoted MQTT topic
                            filters. <code>WHERE</code> is optional.
                            <code> SELECT *</code> includes metadata and the whole <code>payload</code> as text,
                            but does not expand JSON paths.
                            Use <code>AS</code> to rename a JSON key, for example
                            <code> payload.temperature AS temp</code>.</p>
                        <p>Rules may select different fields. Each message produces at most one request:
                            its JSON contains only the fields of the first rule whose topic filter
                            matches and whose WHERE is true (or omitted). Fields from other rules are
                            not merged or filled with nulls.</p>
                        <p><a href="/xmq_doc_webhook#webhook-sql-where" target="_blank" rel="noopener noreferrer">
                            WHERE syntax and examples (opens in a new tab)
                        </a></p>
                    </div> : setting.name === "log_sql_rule" && isLogSource ? <div>
                        <p><code>logs</code> is the only table available in <code>FROM</code>.
                            It represents broker log records, not a table in the destination database.
                            <code> SELECT * FROM logs</code> returns all four fields.</p>
                        <table className="mqttSqlHelpFields">
                            <thead><tr><th>Field</th><th>Value</th></tr></thead>
                            <tbody>
                                <tr><td><code>timestamp</code></td><td>Export timestamp (ISO 8601, UTC)</td></tr>
                                <tr><td><code>level</code></td><td>Severity: debug, info, warning or error</td></tr>
                                <tr><td><code>subject</code></td><td>Log category, for example server_connections</td></tr>
                                <tr><td><code>message</code></td><td>Log message text</td></tr>
                            </tbody>
                        </table>
                        <p>Use these fields in <code>SELECT</code> or optional <code>WHERE</code>.
                            The level and subject controls do not apply in SQL mode. The log source
                            includes broker log categories, not internal <code>[extensions]</code> diagnostics.</p>
                        <pre style={{whiteSpace: "pre-wrap"}}>
                            {"SELECT timestamp, message FROM logs\nWHERE level = 'error'"}
                        </pre>
                        <p><a href="/xmq_doc_webhook#webhook-sql-where" target="_blank" rel="noopener noreferrer">
                            WHERE syntax and examples (opens in a new tab)
                        </a></p>
                    </div> : setting.name === "log_template" && isLogSource ? <div>
                        <p>Quote string placeholders in JSON. Values are escaped and unknown
                            placeholders stay unchanged. SQL rules use SELECT instead.</p>
                        <pre style={{whiteSpace: "pre-wrap"}}>
                            {'{"level":"{{level}}","subject":"{{subject}}","message":"{{message}}"}'}
                        </pre>
                        <p>Available placeholders: <code>{"{{id}}, {{source}}, {{timestamp}}, {{level}}, {{subject}}, {{message}}, {{topic}}"}</code>.
                            Source is <code>log</code>; topic is empty. A blank template uses the default
                            JSON with topic, message and level.</p>
                    </div> : setting.name === "insert_sql" && isExporter ? <div>
                        <p>The destination table must already exist. Bind the complete JSON body
                            with <code>:fields</code>; this parameter is required.</p>
                        <pre style={{whiteSpace: "pre-wrap"}}>
                            {"INSERT INTO exported (body) VALUES (:fields)"}
                        </pre>
                        <p>In SQL mode, use a selected name or alias as another parameter, e.g.
                            <code> SELECT client_id AS client</code> provides <code>:client</code>.
                            In template mode, template values are available by name.
                            Additional bindings are passed as strings.</p>
                    </div> : setting.name === "mqtt_template" && isMqttSource ? <div>
                        <p>Insert a field as <code>{"{{field_name}}"}</code>, without spaces.
                            Values are JSON-escaped, but the editor does not add quotes.</p>
                        <pre style={{whiteSpace: "pre-wrap"}}>
                            {'{"topic":"{{topic}}","body":"{{payload_base64}}","qos":{{qos}},"retain":{{retain}}}'}
                        </pre>
                        <table className="mqttSqlHelpFields">
                            <thead><tr><th>Placeholder</th><th>Value</th></tr></thead>
                            <tbody>{[
                                ["id", "Webhook record ID"],
                                ["source", 'Always "message"'],
                                ["timestamp", "Webhook timestamp (ISO 8601)"],
                                ["topic", "MQTT topic name"],
                                ["qos", "MQTT QoS: 0, 1 or 2"],
                                ["retain", "MQTT retain flag: true or false"],
                                ["client_id", "Publisher client ID"],
                                ["username", "Publisher username"],
                                ["payload", "Message body as UTF-8 text, without Base64"],
                                ["payload_base64", "Message body encoded as Base64"],
                                ["message", "Same as payload_base64"],
                                ["level", 'Always "message" for MQTT']
                            ].map(([field, meaning]) => <tr key={field}>
                                <td><code>{`{{${field}}}`}</code></td><td>{meaning}</td>
                            </tr>)}</tbody>
                        </table>
                        <p>Quote <code>{'"{{payload}}"'}</code> to insert a JSON string, not an object.
                            If the body is not valid UTF-8, use <code>{"{{payload_base64}}"}</code>;
                            a text template skips such messages.</p>
                        <p>Put quotes around string fields. Leave <code>qos</code> and <code>retain</code>
                            {" "}unquoted for JSON numbers and booleans. Unknown placeholders stay unchanged.
                            A blank template uses the default JSON with topic, Base64 message and
                            level. In SQL rules mode, SELECT builds the body instead.</p>
                    </div> : null}/> : null}
            </div>
        </div>;
    }

    renderButtons(extension) {
        if (extension.sink) {
            return null;
        }
        // Offline: extensions live inside the MQTT server, so there is nothing to act on. The page
        // still shows what is configured, which is what somebody diagnosing a stopped broker wants.
        if (!this.state.running) {
            return <p className="extensionNote">
                The MQTT server is stopped, so nothing here can be applied or switched. Start it on
                the Dashboard.
            </p>;
        }

        return <div className="extensionButtons">
            {/* Always here, disabled until there is something to apply: a button that appears only
                on change moves the page under the hand, and its absence reads as a missing feature
                rather than as "nothing to do". */}
            <button className="inputScreenSaveButton"
                    disabled={this.state.busy || !this.hasChanges() || this.invalidSettings().length > 0}
                    onClick={this.apply}>Apply</button>

            {/* Named rather than left to the red field alone: the Apply button being disabled has
                to say why, or the page looks broken. */}
            {this.invalidSettings().length
                ? <p className="extensionNote extensionValueError">
                    Fix the settings marked in red before applying. Their format is declared by the
                    extension, and the broker refuses values that do not match it.
                  </p>
                : null}

            {/* A required extension gets neither button: without it the broker does not serve MQTT
                at all, so "Disable" here would be a way to stop serving that does not say so. */}
            {extension.required
                ? <p className="extensionNote">
                    Required by the configuration: the broker does not serve MQTT without it, so it
                    cannot be switched off here. Change <code>required</code> in <code>{extension.source}</code> if
                    that is what is meant.
                </p>
                : extension.running
                    ? <button className="modalCancelButton" disabled={this.state.busy}
                              onClick={() => this.switchTo(false)}>Disable</button>
                    : <button className="modalOkButton" disabled={this.state.busy}
                              onClick={() => this.switchTo(true)}>Enable</button>}
        </div>;
    }

    renderSelected() {
        const extension = this.selected();
        if (!extension) {
            return null;
        }

        // Two panels, because they answer two different questions: what this extension is and what
        // it is doing, then what it takes. Run together they read as one long list in which the
        // editable half is not obviously editable.
        return <div className="extensionPage">
            <div className="extensionPanel">
                <div className="extensionPanelTitle">
                    {extension.display_name || ExtensionsScreen.humanName(extension.name)}
                </div>
                <div className="extensionPanelBody">
                    {extension.description
                        ? <p className="extensionAbout">{extension.description}</p>
                        : null}
                    {this.renderDashboard(extension)}
                </div>
            </div>

            <div className="extensionPanel">
                <div className="extensionPanelTitle">Settings</div>
                <div className="extensionPanelBody">
                    {extension.sink && <p className="extensionNote">
                        Configure this destination under Delivery in <code>message_export</code> or{" "}
                        <code>log_export</code>. This page only shows the available settings.
                    </p>}
                    {this.presetsOf(extension).length ? this.renderPresets(extension) : null}
                    {(extension.settings || []).length
                        ? this.renderFields(extension)
                        : <p className="extensionNote">
                            This extension declares no settings and none are configured for it.
                        </p>}

                    {this.renderButtons(extension)}
                </div>
            </div>
        </div>;
    }

    render() {
        return <div>
            <PageTitle>Extensions</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Extensions are libraries the broker loads at start-up. Each one declares what it
                    does - watch events, decide whether a client may connect, decide what a client
                    may publish and subscribe to - and the broker builds only the machinery for what
                    is declared.
                </p>
                <p>
                    Everything configured is listed here, running or not. Extensions are grouped by
                    what they do; each row under one is an independent settings set. An export can
                    be added several times to write to different destinations - each set keeps its
                    own filter, sink, queue and dead-letter topic. Select one to see what it is
                    doing and to change what it takes.
                </p>
            </div>

            {this.renderList()}
            {this.renderSelected()}
        </div>;
    }
}
