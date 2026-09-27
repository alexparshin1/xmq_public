import React from "react";
import {errorWindow, informationWindow} from "../components/MessageWindow";
import ControlAPI from "../ControlAPI";
import "./InputScreen.css";
import "./ExtensionsScreen.css";
import PageTitle from "../components/PageTitle";
import HelpButton from "../components/HelpButton";
import PasswordInput from "../components/PasswordInput";

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
        running: true,    // whether the MQTT server is up; extensions live inside it
        busy: false
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
    refresh = (message) => {
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
                this.setState({extensions: data.list ? data.list : [], edited: {}, busy: false});
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

    selected() {
        return this.state.extensions.find(one => one.name === this.state.selected);
    }

    /// What a setting holds right now: what the operator typed, or what the broker reported.
    valueOf(setting) {
        const edited = this.state.edited[setting.name];
        return edited === undefined ? (setting.value === undefined ? "" : setting.value) : edited;
    }

    hasChanges() {
        return Object.keys(this.state.edited).length > 0;
    }

    onSettingChange = (name, value) => {
        this.setState(state => ({edited: {...state.edited, [name]: value}}));
    }

    apply = () => {
        const extension = this.selected();
        if (!extension) {
            return;
        }

        // Every setting is sent, not only the changed ones: the server replaces the block wholesale
        // so that a key removed here is removed there, and sending a partial list would make
        // removal impossible.
        const settings = (extension.settings || []).map(setting => ({
            name: setting.name,
            value: this.valueOf(setting)
        }));

        this.setState({busy: true});
        ControlAPI.asyncMakeAPICall("ExtensionControl",
                                    {action: "set", name: extension.name, settings: settings})
            .then(data => {
                this.setState({busy: false});
                if (!ExtensionsScreen.succeeded(data)) {
                    this.restore(data);
                    return;
                }
                this.refresh(data.message);
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
                                    {action: on ? "enable" : "disable", name: extension.name})
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

        return <table className="extensionList">
            <thead>
            <tr>
                <th>Extension</th>
                <th>Version</th>
                <th>State</th>
                <th>Role</th>
                <th>What it is for</th>
            </tr>
            </thead>
            <tbody>
            {this.state.extensions.map(extension =>
                <tr key={extension.name}
                    className={extension.name === this.state.selected ? "extensionRowSelected" : ""}
                    onClick={() => this.setState({selected: extension.name, edited: {}})}>
                    <td>{extension.name}</td>
                    <td>{extension.version || "—"}</td>
                    <td className={extension.running ? "extensionRunning" : "extensionStopped"}>
                        {extension.running ? "running" : "stopped"}
                        {extension.required ? " (required)" : ""}
                    </td>
                    <td>{this.capabilitiesOf(extension)}</td>
                    <td>{extension.description || "—"}</td>
                </tr>)}
            </tbody>
        </table>;
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

        return <div className="extensionForm">
            {rows.map(([label, value, help]) => this.renderRow(label, value, help))}
        </div>;
    }

    renderSetting(setting) {
        const value = this.valueOf(setting);
        const change = event => this.onSettingChange(setting.name, event.target.value);

        let control;
        if (setting.type === "choice") {
            control = <select value={value} onChange={change}>
                {/* The stored value first, even when the extension no longer offers it: a value
                    silently replaced by the first of the list is a setting changed without asking. */}
                {value && !(setting.choices || "").split(",").map(one => one.trim()).includes(value)
                    ? <option key={value} value={value}>{value}</option>
                    : null}
                {(setting.choices || "").split(",").map(choice => choice.trim()).filter(Boolean)
                    .map(choice => <option key={choice} value={choice}>{choice}</option>)}
            </select>;
        } else if (setting.type === "boolean") {
            control = <input type="checkbox" checked={value === "true"}
                             onChange={event => this.onSettingChange(setting.name,
                                                                     event.target.checked ? "true" : "false")}/>;
        } else if (setting.type === "integer") {
            control = <input type="number" value={value} onChange={change}/>;
        } else if (setting.type === "secret") {
            // The broker never sends a secret, only whether one is set. Leaving the mask alone
            // leaves the stored value alone; typing over it replaces it.
            control = <PasswordInput value={value} onChange={change} placeholder="not set"/>;
        } else {
            control = <input type="text" value={value} onChange={change}/>;
        }

        // An undeclared key is worth explaining rather than hiding: a misspelling is the commonest
        // reason a setting quietly does nothing.
        const help = setting.declared === false
            ? "Set here, but this extension does not declare a setting of that name - usually a " +
              "misspelling, and then it has no effect."
            : (setting.description || "") +
              (setting.default_value ? (setting.description ? " " : "") +
                                       "Default: " + setting.default_value + "." : "");

        return <div className="extensionRow" key={setting.name}>
            <label>
                {(setting.label || setting.name)}
                {setting.required ? <b className="extensionRequired"> *</b> : null}:
            </label>
            <div className="extensionValue">{control}</div>
            <div className="extensionHelp">
                {help ? <HelpButton title={setting.label || setting.name} text={help}/> : null}
            </div>
        </div>;
    }

    renderButtons(extension) {
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
                    disabled={this.state.busy || !this.hasChanges()}
                    onClick={this.apply}>Apply</button>

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
                <div className="extensionPanelTitle">{extension.name}</div>
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
                    {(extension.settings || []).length
                        ? <div className="extensionForm">
                            {(extension.settings || []).map(setting => this.renderSetting(setting))}
                        </div>
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
                    Everything configured is listed here, running or not. Select one to see what it
                    is doing and to change what it takes.
                </p>
            </div>

            {this.renderList()}
            {this.renderSelected()}
        </div>;
    }
}
