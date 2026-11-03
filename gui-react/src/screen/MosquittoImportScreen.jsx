import React from "react";
import {errorWindow, informationWindow} from "../components/MessageWindow";
import ControlAPI from "../ControlAPI";
import FileLoad from "../components/FileLoad";
import PageTitle from "../components/PageTitle";
import "./InputScreen.css";
import "./PermissionsScreen.css";

/**
 * Import from Mosquitto.
 *
 * The files are read by the browser, as the person using it, and sent to the server as content:
 * the broker never opens /etc/mosquitto with its own privileges, and it cannot be used to read a
 * file the administrator could not read. Either file may be left empty; an empty input means that
 * part is not imported.
 */
export default class MosquittoImportScreen extends React.Component {

    state = {
        passwordName: "",
        passwordContent: "",
        aclName: "",
        aclContent: "",
        aclFormat: "acl",
        passwordPlan: null,
        aclPlan: null,
        aclDefaultDeny: false,
        busy: false,
        result: null
    }

    static succeeded(data) {
        if (!data) {
            return false;
        }
        const error = ControlAPI.apiError(data);
        if (error) {
            errorWindow(error);
            return false;
        }
        return true;
    }

    static parsePlan(data) {
        if (!data || !data.plan) {
            return null;
        }
        try {
            return JSON.parse(data.plan);
        } catch (error) {
            errorWindow("The server's import plan could not be read.");
            return null;
        }
    }

    passwordLoaded = (name, content) => {
        // A clear field after a load is easier to notice than a stale plan for another file.
        this.setState({passwordName: name, passwordContent: content, passwordPlan: null, result: null});
    }

    aclLoaded = (name, content) => {
        // The extension is a hint: a dynamic-security file is JSON, an ACL file is not. The
        // selector beside the input can still be changed if the name lies.
        const aclFormat = /\.json$/i.test(name) ? "dynsec" : "acl";
        this.setState({aclName: name, aclContent: content, aclFormat, aclPlan: null, result: null});
    }

    preview = async () => {
        const {passwordContent, aclContent} = this.state;
        if (!passwordContent.trim() && !aclContent.trim()) {
            errorWindow("Choose a password file, an ACL file, or both.");
            return false;
        }

        this.setState({busy: true, passwordPlan: null, aclPlan: null, result: null});
        let passwordPlan = null;
        let aclPlan = null;
        let aclDefaultDeny = false;

        if (passwordContent.trim()) {
            const data = await ControlAPI.asyncMakeAPICall("UserControl",
                {action: "import-plan", content: passwordContent});
            if (!MosquittoImportScreen.succeeded(data)) {
                this.setState({busy: false});
                return false;
            }
            passwordPlan = MosquittoImportScreen.parsePlan(data);
        }

        if (aclContent.trim()) {
            const data = await ControlAPI.asyncMakeAPICall("PermissionsControl",
                {action: "import-plan", format: this.state.aclFormat, content: aclContent});
            if (!MosquittoImportScreen.succeeded(data)) {
                this.setState({busy: false});
                return false;
            }
            aclPlan = MosquittoImportScreen.parsePlan(data);
            aclDefaultDeny = data.default_deny === true;
        }

        this.setState({busy: false, passwordPlan, aclPlan, aclDefaultDeny});
        return {passwordPlan, aclPlan, aclDefaultDeny};
    }

    hasConflicts() {
        return !!(this.state.aclPlan && this.state.aclPlan.counts &&
                  this.state.aclPlan.counts.conflicts > 0);
    }

    importAll = async () => {
        // Always planned against the content that is about to be sent: a file can be chosen again
        // after a preview, and applying a stale plan to new content is exactly the mistake this
        // page exists to avoid. The returned plan is used, not the state, which React may not have
        // applied yet.
        const preview = await this.preview();
        if (!preview) {
            return;
        }
        if (preview.aclPlan && preview.aclPlan.counts && preview.aclPlan.counts.conflicts > 0) {
            errorWindow("The ACL has conflicts. Open Permissions, Import tab, and resolve them there.");
            return;
        }

        this.setState({busy: true});
        const result = {users: null, rules: null};

        if (this.state.passwordContent.trim()) {
            const data = await ControlAPI.asyncMakeAPICall("UserControl",
                {action: "import-apply", content: this.state.passwordContent});
            if (!MosquittoImportScreen.succeeded(data)) {
                this.setState({busy: false});
                return;
            }
            result.users = data;
        }

        if (this.state.aclContent.trim()) {
            const data = await ControlAPI.asyncMakeAPICall("PermissionsControl",
                {action: "import-apply", format: this.state.aclFormat, content: this.state.aclContent,
                 resolution: "reject"});
            if (!MosquittoImportScreen.succeeded(data)) {
                this.setState({busy: false});
                return;
            }
            result.rules = data;
        }

        this.setState({busy: false, result});
        const parts = [];
        if (result.users) {
            parts.push(result.users.new_count + " account(s) added");
        }
        if (result.rules) {
            parts.push(result.rules.new_count + " rule(s) added");
        }
        informationWindow(parts.join(", ") + ".", "Import applied");
    }

    renderPasswordPlan() {
        const plan = this.state.passwordPlan;
        if (!plan) {
            return null;
        }
        const counts = plan.counts || {};
        const list = (title, entries) => entries && entries.length
            ? <div>
                <strong>{title} ({entries.length})</strong>
                <ul>{entries.map(entry =>
                    <li key={entry.username + entry.status}>
                        {entry.username}{entry.reason ? " - " + entry.reason : ""}
                    </li>)}</ul>
            </div>
            : null;
        return <div className="inputScreenNote" style={{maxWidth: "760px"}}>
            <p>
                {counts.new || 0} new account(s), {counts.existing || 0} already present,
                {" "}{counts.unsupported || 0} with a verifier that cannot be checked,
                {" "}{counts.invalid || 0} unusable line(s).
            </p>
            {list("New accounts", plan.new)}
            {list("Already present, not changed", plan.existing)}
            {list("Not imported - the password has to be set again", plan.unsupported)}
            {list("Unusable lines", plan.invalid)}
        </div>;
    }

    renderAclPlan() {
        const plan = this.state.aclPlan;
        if (!plan) {
            return null;
        }
        const counts = plan.counts || {};
        const sourceRules = (counts.new || 0) + (counts.unchanged || 0) +
            (counts.redundant || 0) + (counts.conflicts || 0);
        const conflicts = plan.conflicts || [];
        return <div className="inputScreenNote" style={{maxWidth: "760px"}}>
            <p>
                {counts.new || 0} new rule(s), {counts.unchanged || 0} already present,
                {" "}{counts.redundant || 0} covered by an existing rule,
                {" "}{counts.conflicts || 0} conflict(s), {plan.skipped ? plan.skipped.length : 0} not
                imported.
            </p>
            {sourceRules > 0
                ? <p>
                    <strong>This import {this.state.aclDefaultDeny ? "keeps" : "turns on"} deny-by-default:</strong>
                    {" "}a client with no matching rule will be denied.
                </p>
                : <p>
                    The ACL file has no rules, so the current default-deny setting is left unchanged.
                </p>}
            {conflicts.length
                ? <div className="inputScreenWarning">
                    The ACL has conflicts; resolve them on the Permissions page's Import tab.
                </div>
                : null}
            {plan.skipped && plan.skipped.length
                ? <div>
                    <strong>Not imported ({plan.skipped.length})</strong>
                    <ul>{plan.skipped.map((note, at) => <li key={at}>{note}</li>)}</ul>
                </div>
                : null}
        </div>;
    }

    renderResult() {
        const result = this.state.result;
        if (!result) {
            return null;
        }
        return <div className="inputScreenNote">
            <strong>Import finished.</strong>
            {result.users ? " " + result.users.new_count + " account(s) added." : ""}
            {result.rules ? " " + result.rules.new_count + " rule(s) added." : ""}
        </div>;
    }

    render() {
        return <div>
            <PageTitle>Import from Mosquitto</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Copies the accounts and topic rules of a Mosquitto broker into this one. The
                    files are read by this browser, not by the XMQ server: choose them from the
                    machine where the interface is open. If Mosquitto runs elsewhere, copy
                    <code>password_file</code> and <code>acl_file</code> here first. Either file may
                    be left empty, and then that part is not imported.
                </p>
                <p>
                    A <code>dynamic-security.json</code> can be chosen as the ACL file; a
                    <code>.json</code> name selects that format automatically. Nothing is written
                    until <b>Import</b> is pressed. The password file may use
                    Mosquitto 2.0 <code>$7$</code> verifiers or clear-text lines; 1.6
                    <code>$6$</code> and 2.1 <code>argon2id</code> verifiers are reported and need
                    the password to be set again.
                </p>
                <p>
                    Imported accounts can also sign in to this interface, with the rights of the
                    groups they are put in - none of them become administrators unless an
                    administrator adds them to that group.
                </p>
            </div>

            <div className="inputScreenRow">
                <label className="inputScreenLabel">Password file:</label>
                <FileLoad onLoad={this.passwordLoaded}/>
                {this.state.passwordName
                    ? <span className="inputScreenHint">{this.state.passwordName}</span>
                    : <span className="inputScreenHint">Usually /etc/mosquitto/passwd</span>}
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">ACL file:</label>
                <FileLoad onLoad={this.aclLoaded}/>
                {this.state.aclName
                    ? <span className="inputScreenHint">{this.state.aclName}</span>
                    : <span className="inputScreenHint">Usually /etc/mosquitto/aclfile</span>}
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">ACL format:</label>
                <select className="inputScreenSelect" value={this.state.aclFormat}
                        onChange={e => this.setState({aclFormat: e.target.value, aclPlan: null})}>
                    <option value="acl">Mosquitto ACL file</option>
                    <option value="dynsec">dynamic-security.json</option>
                </select>
                <span className="inputScreenHint">
                    A .json file switches this by itself; the format tells the parser which file it is.
                </span>
            </div>

            <div className="inputScreenRow">
                <button className="inputScreenSaveButton inputScreenActionButton"
                        disabled={this.state.busy} onClick={this.preview}>Preview</button>
                <button className="inputScreenSaveButton inputScreenActionButton"
                        disabled={this.state.busy || this.hasConflicts()}
                        onClick={this.importAll}>Import</button>
            </div>

            {this.renderPasswordPlan()}
            {this.renderAclPlan()}
            {this.renderResult()}
        </div>;
    }
}
