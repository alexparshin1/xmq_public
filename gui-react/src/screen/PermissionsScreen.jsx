import React from "react";
import {confirmationWindow, errorWindow, informationWindow} from "../components/MessageWindow";
import ControlAPI from "../ControlAPI";
import FileLoad from "../components/FileLoad";
import PageTitle from "../components/PageTitle";
import {subjectLabel} from "../components/DialogWindow";
import "./InputScreen.css";
import "./PermissionsScreen.css";

/**
 * Whether a rule's topic filter is one the server accepts.
 *
 * The same rules the control API applies, checked here so a typo is reported before the dialog
 * closes: no empty level, `+` and `#` each a whole level, and `#` the last one. Empty levels are
 * legal MQTT, but a filter that names one is a typo far more often than an intention, and the
 * import refuses them for the same reason.
 */
export function validTopicFilter(filter) {
    const text = String(filter || "");
    if (!text) {
        return false;
    }
    const levels = text.split("/");
    for (let at = 0; at < levels.length; ++at) {
        const level = levels[at];
        if (!level) {
            return false;
        }
        if (level === "+") {
            continue;
        }
        if (level === "#") {
            return at === levels.length - 1;
        }
        if (level.includes("#") || level.includes("+")) {
            return false;
        }
    }
    return true;
}

/**
 * Permissions screen: a policy per subject, a way to try it, and an import.
 *
 * A policy is the ordered list of rules that applies to one user, one group or anonymous clients.
 * The list is checked from the top and the first matching rule decides, as in Mosquitto: an allow
 * above a deny lets the deny's topic through. "read" from Mosquitto is subscribe here, because there
 * is no separate permission for receiving what one is subscribed to.
 *
 * The import is a plan first and a write second. Nothing is stored while a conflict - an allow and
 * a deny that overlap - is unresolved, and a second import of the same file changes nothing.
 */
export default class PermissionsScreen extends React.Component {

    state = {
        rows: [],
        subjectOptions: {groups: [], users: []},
        defaultDeny: false,
        policyBusy: false,
        policyEditor: null,

        testUsername: "",
        testClientId: "",
        testTopic: "",
        testAction: "subscribe",
        testResult: null,
        testBusy: false,

        importFormat: "acl",
        importName: "",
        importContent: "",
        importPlan: null,
        importResolution: "reject",
        importBusy: false,

        tab: "rules"
    }

    componentDidMount() {
        this.readSubjects();
        this.readRules();
    }

    /**
     * Fills the subject pickers with the groups and accounts the server knows.
     *
     * A refusal is not reported here: the boxes are combo boxes and still accept a typed value, so
     * a page whose lists did not arrive is less useful but not broken. Reporting it as well would
     * put an error in front of a page that has already loaded.
     */
    readSubjects() {
        return Promise.all([
            ControlAPI.asyncMakeAPICall("UserGroupControl", {action: "list"}),
            ControlAPI.asyncMakeAPICall("UserControl", {action: "list"})
        ]).then(([groups, users]) => {
            const groupNames = groups && !ControlAPI.apiError(groups) && groups.list
                ? groups.list.map(group => group.name).filter(name => name) : [];
            if (!groupNames.includes("anonymous")) {
                groupNames.push("anonymous");
            }
            const userNames = users && !ControlAPI.apiError(users) && users.list
                ? users.list.map(user => user.username).filter(name => name) : [];
            this.setState({subjectOptions: {groups: groupNames, users: userNames}});
        });
    }

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

    readRules() {
        return ControlAPI.asyncMakeAPICall("PermissionsControl", {action: "list"})
            .then(data => {
                if (!PermissionsScreen.succeeded(data)) {
                    return false;
                }
                this.setState({
                    rows: (data.rules ? data.rules : []).map(rule => ({
                        ...rule,
                        group: rule.group || "",
                        filter: rule.filter || "",
                        access: rule.deny ? "deny" : "allow",
                        pattern: rule.pattern === true,
                        order: rule.order === undefined ? 0 : rule.order
                    })),
                    defaultDeny: data.default_deny === true
                });
                return true;
            });
    }

    setDefaultDeny = deny => {
        this.setState({policyBusy: true});
        return ControlAPI.asyncMakeAPICall("PermissionsControl", {action: "set-policy", default_deny: deny})
            .then(data => {
                if (!PermissionsScreen.succeeded(data)) {
                    this.setState({policyBusy: false});
                    return;
                }
                this.setState({defaultDeny: data.default_deny === true, policyBusy: false});
            });
    }

    /// One entry per subject, each holding that subject's rules in evaluation order.
    policies() {
        const bySubject = new Map();
        for (const rule of this.state.rows) {
            const key = (rule.pattern ? "pattern:" : "subject:") + rule.group;
            let policy = bySubject.get(key);
            if (!policy) {
                policy = {key, subject: rule.group, pattern: rule.pattern, rules: []};
                bySubject.set(key, policy);
            }
            policy.rules.push(rule);
        }
        const policies = [...bySubject.values()];
        for (const policy of policies) {
            policy.rules.sort((first, second) => first.order - second.order);
        }
        policies.sort((first, second) => {
            const left = first.pattern ? "all clients" : subjectLabel(first.subject);
            const right = second.pattern ? "all clients" : subjectLabel(second.subject);
            return left.localeCompare(right);
        });
        return policies;
    }

    policyTitle(policy) {
        if (policy.pattern) {
            return "All clients";
        }
        return subjectLabel(policy.subject);
    }

    addPolicy = () => {
        this.setState({
            policyEditor: {
                subject: "",
                pattern: false,
                original: null,
                entries: [{deny: false, action: "subscribe", filter: ""}],
                error: ""
            }
        });
    }

    editPolicy = policy => {
        this.setState({
            policyEditor: {
                subject: policy.subject,
                pattern: policy.pattern,
                original: policy,
                entries: policy.rules.map(rule => ({
                    deny: rule.deny === true,
                    action: rule.action,
                    filter: rule.filter
                })),
                error: ""
            }
        });
    }

    closePolicyEditor = () => this.setState({policyEditor: null});

    updatePolicyEntry = (index, patch) => {
        this.setState(state => {
            const editor = state.policyEditor;
            if (!editor) {
                return null;
            }
            const entries = editor.entries.map((entry, at) => at === index ? {...entry, ...patch} : entry);
            return {policyEditor: {...editor, entries, error: ""}};
        });
    }

    addPolicyEntry = () => {
        this.setState(state => {
            const editor = state.policyEditor;
            if (!editor) {
                return null;
            }
            return {policyEditor: {
                ...editor,
                entries: [...editor.entries, {deny: false, action: "subscribe", filter: ""}]
            }};
        });
    }

    removePolicyEntry = index => {
        this.setState(state => {
            const editor = state.policyEditor;
            if (!editor) {
                return null;
            }
            return {policyEditor: {
                ...editor,
                entries: editor.entries.filter((entry, at) => at !== index)
            }};
        });
    }

    movePolicyEntry = (index, delta) => {
        this.setState(state => {
            const editor = state.policyEditor;
            if (!editor) {
                return null;
            }
            const target = index + delta;
            if (target < 0 || target >= editor.entries.length) {
                return null;
            }
            const entries = [...editor.entries];
            [entries[index], entries[target]] = [entries[target], entries[index]];
            return {policyEditor: {...editor, entries}};
        });
    }

    /**
     * Says what is wrong with the policy being edited, or null.
     *
     * The order is the policy, so a duplicate entry is refused: the second one could never decide
     * anything, and keeping it would only make the list ambiguous.
     */
    policyProblem(editor) {
        if (!editor.subject) {
            return "Choose the user or group the policy applies to.";
        }
        if (editor.entries.length === 0) {
            return "A policy needs at least one rule.";
        }
        const seen = new Set();
        for (const entry of editor.entries) {
            const filter = String(entry.filter || "").trim();
            if (!validTopicFilter(filter)) {
                return "'" + filter + "' is not a valid topic filter: levels cannot be empty, "
                    + "+ and # must each be a whole level, and # must be the last.";
            }
            const key = entry.action + "|" + (entry.deny ? "1" : "0") + "|" + filter;
            if (seen.has(key)) {
                return "The policy repeats " + (entry.deny ? "deny " : "allow ") + entry.action
                    + " " + filter + ".";
            }
            seen.add(key);
        }
        return null;
    }

    savePolicy = async () => {
        const editor = this.state.policyEditor;
        if (!editor) {
            return;
        }
        const problem = this.policyProblem(editor);
        if (problem) {
            this.setState({policyEditor: {...editor, error: problem}});
            return;
        }

        this.setState({policyBusy: true});
        const data = await ControlAPI.asyncMakeAPICall("PermissionsControl", {
            action: "replace-policy",
            subject: editor.subject,
            subject_pattern: editor.pattern,
            policy_rules: editor.entries.map(entry => ({
                // The subject is sent on every entry only because the schema requires it on an
                // AclRule; the server takes the subject from the field above.
                group: editor.subject,
                action: entry.action,
                deny: entry.deny,
                filter: String(entry.filter).trim()
            }))
        });
        this.setState({policyBusy: false});
        if (!PermissionsScreen.succeeded(data)) {
            return;
        }
        this.setState({policyEditor: null});
        this.readRules();
    }

    removePolicy = policy => {
        confirmationWindow("Remove every rule for " + this.policyTitle(policy) + "?", "Remove policy")
            .then(confirmed => {
                if (!confirmed) {
                    return;
                }
                ControlAPI.asyncMakeAPICall("PermissionsControl", {
                    action: "replace-policy",
                    subject: policy.subject,
                    subject_pattern: policy.pattern,
                    policy_rules: []
                }).then(data => {
                    if (PermissionsScreen.succeeded(data)) {
                        this.readRules();
                    }
                });
            });
    }

    runTest = () => {
        const topic = this.state.testTopic.trim();
        if (!topic) {
            errorWindow("A test needs a topic.");
            return;
        }
        this.setState({testBusy: true, testResult: null});
        ControlAPI.asyncMakeAPICall("PermissionsControl", {
            action: "test",
            username: this.state.testUsername.trim(),
            client_id: this.state.testClientId.trim(),
            topic: topic,
            acl_action: this.state.testAction
        }).then(data => {
            if (!PermissionsScreen.succeeded(data)) {
                this.setState({testBusy: false});
                return;
            }
            this.setState({
                testBusy: false,
                testResult: {
                    decision: data.decision || "not_handled",
                    explanation: data.explanation || ""
                }
            });
        });
    }

    previewImport = () => {
        const content = this.state.importContent;
        if (!content.trim()) {
            errorWindow("Choose a file or paste its contents first.");
            return;
        }
        this.setState({importBusy: true, importPlan: null});
        ControlAPI.asyncMakeAPICall("PermissionsControl", {
            action: "import-plan",
            format: this.state.importFormat,
            content: content
        }).then(data => {
            if (!PermissionsScreen.succeeded(data)) {
                this.setState({importBusy: false});
                return;
            }
            let plan = null;
            try {
                plan = data.plan ? JSON.parse(data.plan) : null;
            } catch (error) {
                errorWindow("The server's import plan could not be read.");
            }
            this.setState({
                importBusy: false,
                importPlan: plan,
                importResolution: plan && plan.conflicts && plan.conflicts.length ? "existing" : "reject"
            });
        });
    }

    applyImport = () => {
        this.setState({importBusy: true});
        ControlAPI.asyncMakeAPICall("PermissionsControl", {
            action: "import-apply",
            format: this.state.importFormat,
            content: this.state.importContent,
            resolution: this.state.importResolution
        }).then(data => {
            this.setState({importBusy: false});
            if (!PermissionsScreen.succeeded(data)) {
                return;
            }
            const plan = this.state.importPlan;
            const added = plan && plan.counts ? plan.counts.new : 0;
            informationWindow("Imported " + added + " new rule(s).", "Import applied");
            this.setState({importPlan: null, importName: "", importContent: ""});
            this.readRules();
        });
    }

    importFileLoaded = (name, content) => {
        const format = /\.json$/i.test(name) ? "dynsec" : this.state.importFormat;
        this.setState({importName: name, importContent: content, importFormat: format});
    }

    renderDecision() {
        const result = this.state.testResult;
        if (!result) {
            return null;
        }
        const wording = {
            allow: "Allowed",
            deny: "Denied",
            not_handled: "Not covered by any rule"
        }[result.decision] || result.decision;
        const className = result.decision === "deny" ? "inputScreenWarning"
            : (result.decision === "allow" ? "inputScreenHint" : "inputScreenNote");
        return <div className={className}>
            <strong>{wording}.</strong> {result.explanation}
        </div>;
    }

    renderPolicyEditor() {
        const editor = this.state.policyEditor;
        if (!editor) {
            return null;
        }
        const subjects = [];
        for (const group of this.state.subjectOptions.groups) {
            subjects.push({value: group, label: subjectLabel(group)});
        }
        for (const user of this.state.subjectOptions.users) {
            subjects.push({value: "user:" + user, label: subjectLabel("user:" + user)});
        }
        return <div className="modalBackground">
            <div className="permissionsPolicyDialog">
                <div className="title">{editor.original ? "Edit Policy" : "Add Policy"}</div>
                <div className="permissionsPolicyBody">
                    <div className="permissionsPolicySubject">
                        <label className="inputScreenLabel">Applies to:</label>
                        {editor.original
                            ? <span className="permissionsPolicySubjectName">
                                {editor.pattern ? "all clients" : subjectLabel(editor.subject)}
                              </span>
                            : <select className="inputScreenSelect" value={editor.subject}
                                      onChange={e => this.setState(state => ({
                                          policyEditor: {...state.policyEditor, subject: e.target.value, error: ""}
                                      }))}>
                                <option value="">Choose a user or group</option>
                                {subjects.map(subject =>
                                    <option key={subject.value} value={subject.value}>{subject.label}</option>)}
                              </select>}
                    </div>

                    <table className="permissionsPolicyEditorTable">
                        <thead>
                        <tr>
                            <th>Rule</th>
                            <th>Action</th>
                            <th>Topic filter</th>
                            <th/>
                        </tr>
                        </thead>
                        <tbody>
                        {editor.entries.map((entry, index) =>
                            <tr key={index}>
                                <td>
                                    <select value={entry.deny ? "deny" : "allow"}
                                            onChange={e => this.updatePolicyEntry(index, {deny: e.target.value === "deny"})}>
                                        <option value="allow">allow</option>
                                        <option value="deny">deny</option>
                                    </select>
                                </td>
                                <td>
                                    <select value={entry.action}
                                            onChange={e => this.updatePolicyEntry(index, {action: e.target.value})}>
                                        <option value="publish">publish</option>
                                        <option value="subscribe">subscribe</option>
                                    </select>
                                </td>
                                <td>
                                    <input value={entry.filter}
                                           onChange={e => this.updatePolicyEntry(index, {filter: e.target.value})}/>
                                </td>
                                <td className="permissionsPolicyEntryButtons">
                                    <button type="button" disabled={index === 0}
                                            onClick={() => this.movePolicyEntry(index, -1)}>&uarr;</button>
                                    <button type="button" disabled={index === editor.entries.length - 1}
                                            onClick={() => this.movePolicyEntry(index, 1)}>&darr;</button>
                                    <button type="button"
                                            onClick={() => this.removePolicyEntry(index)}>Remove</button>
                                </td>
                            </tr>)}
                        </tbody>
                    </table>
                    <button type="button" className="inputScreenSaveButton inputScreenActionButton"
                            onClick={this.addPolicyEntry}>Add rule</button>
                    {editor.error
                        ? <div className="subjectPickerError">{editor.error}</div>
                        : null}
                </div>
                <div className="modalFooter">
                    <button className="modalOkButton" disabled={this.state.policyBusy}
                            onClick={this.savePolicy}>Ok</button>
                    <button className="modalCancelButton" onClick={this.closePolicyEditor}>Cancel</button>
                </div>
            </div>
        </div>;
    }

    renderPolicies() {
        const policies = this.policies();
        if (policies.length === 0) {
            return <p className="inputScreenNote">No rules yet. Add a policy for a user or group.</p>;
        }
        return <table className="permissionsPolicyTable">
            <thead>
            <tr>
                <th>Applies to</th>
                <th>Policy, checked in order</th>
                <th/>
            </tr>
            </thead>
            <tbody>
            {policies.map(policy =>
                <tr key={policy.key}>
                    <td className="permissionsPolicySubjectName">{this.policyTitle(policy)}</td>
                    <td>
                        <ol className="permissionsPolicyRules">
                            {policy.rules.slice(0, 5).map(rule =>
                                <li key={rule.id}>
                                    <span className={rule.deny ? "permissionsDeny" : "permissionsAllow"}>
                                        {rule.deny ? "deny" : "allow"}
                                    </span>
                                    {" "}
                                    <span className="permissionsAction">{rule.action}</span>
                                    {" "}
                                    <code>{rule.filter}</code>
                                </li>)}
                            {policy.rules.length > 5
                                ? <li className="permissionsPolicyMore">
                                    ... and {policy.rules.length - 5} more
                                  </li>
                                : null}
                        </ol>
                    </td>
                    <td className="permissionsPolicyRowButtons">
                        <button type="button" className="dataTableAddEditButton"
                                onClick={() => this.editPolicy(policy)}>Edit</button>
                        <button type="button" className="dataTableRemoveButton"
                                onClick={() => this.removePolicy(policy)}>Remove</button>
                    </td>
                </tr>)}
            </tbody>
        </table>;
    }

    renderImportPlan() {
        const plan = this.state.importPlan;
        if (!plan) {
            return null;
        }
        const counts = plan.counts || {};
        const conflicts = plan.conflicts || [];
        const list = (title, rules, describe) => rules && rules.length
            ? <div>
                <strong>{title} ({rules.length})</strong>
                <ul>{rules.map((rule, at) => <li key={at}>{describe(rule)}</li>)}</ul>
            </div>
            : null;
        const describe = rule => (rule.pattern ? "all clients" : rule.group) + " " +
            (rule.deny ? "deny " : "allow ") + rule.action + " " + rule.filter;
        return <div className="inputScreenNote" style={{maxWidth: "760px"}}>
            <p>
                {counts.new || 0} new, {counts.unchanged || 0} already present,
                {" "}{counts.redundant || 0} covered by an existing rule,
                {" "}{counts.conflicts || 0} conflict(s).
            </p>
            {list("Conflicts - an allow and a deny overlap", conflicts, conflict =>
                describe(conflict.imported) + " overlaps " + describe(conflict.existing) +
                " on " + conflict.scope)}
            {list("New", plan.new, describe)}
            {list("Already covered by an existing rule, not added", plan.redundant, describe)}
            {list("Not imported", plan.skipped, note => note)}
            {conflicts.length
                ? <div className="inputScreenRow">
                    <label className="inputScreenLabel">Resolve conflicts:</label>
                    <select className="inputScreenSelect" value={this.state.importResolution}
                            onChange={e => this.setState({importResolution: e.target.value})}>
                        <option value="existing">Keep the existing rule</option>
                        <option value="imported">Use the imported rule</option>
                        <option value="skip">Do not import the conflicting rule</option>
                    </select>
                </div>
                : null}
            <button className="inputScreenSaveButton inputScreenActionButton"
                    disabled={this.state.importBusy} onClick={this.applyImport}>
                Apply import
            </button>
        </div>;
    }

    render() {
        const tab = this.state.tab;
        return <div>
            <PageTitle>Permissions</PageTitle>

            {/* Two parts, one screen: what the rules are, and what a rule would answer. Tabs
                rather than one long page because the policy list fills the screen, and the test
                panel below it was found only by scrolling to the bottom. */}
            <div className="permissionsTabs" role="tablist">
                <button type="button" role="tab" aria-selected={tab === "rules"}
                        className={"permissionsTab" + (tab === "rules" ? " permissionsTabActive" : "")}
                        onClick={() => this.setState({tab: "rules"})}>Rules</button>
                <button type="button" role="tab" aria-selected={tab === "test"}
                        className={"permissionsTab" + (tab === "test" ? " permissionsTabActive" : "")}
                        onClick={() => this.setState({tab: "test"})}>Test Access</button>
            </div>

            {tab === "test" ? this.renderTestAccess() : this.renderRules()}
            {this.renderPolicyEditor()}
        </div>;
    }

    renderRules() {
        return <>
            <div className="inputScreenIntro">
                <p>
                    Each row is one user, one group, or anonymous clients, and the list beneath it is
                    that subject's policy. The list is checked from the top and the first matching
                    rule decides: an allow above a deny lets the deny's topic through. A client the
                    rules know but no rule covers is denied when the setting below says so.
                </p>
                <p>
                    These rules decide which topics an account may publish and subscribe to. A
                    subject is <code>user:&lt;name&gt;</code> for one account,
                    <code>group:&lt;name&gt;</code> for a group from the Groups page, or
                    <code>anonymous</code> for clients that connect without a username. The
                    <code>group:</code> prefix is how the page shows a group so it cannot be
                    mistaken for a username; the stored rule keeps the bare name.
                </p>
            </div>

            <div className="inputScreenRow">
                <label className="inputScreenLabel">Clients with no matching rule:</label>
                <select className="inputScreenSelect"
                        value={this.state.defaultDeny ? "deny" : "allow"}
                        disabled={this.state.policyBusy}
                        onChange={e => this.setDefaultDeny(e.target.value === "deny")}>
                    <option value="deny">Deny</option>
                    <option value="allow">Allow</option>
                </select>
                <span className="inputScreenHint">
                    Deny is Mosquitto's default when an ACL file is present; Allow leaves a client
                    the rules do not know to the extensions.
                </span>
            </div>

            {this.renderPolicies()}

            <div className="inputScreenRow">
                <button className="inputScreenSaveButton inputScreenActionButton"
                        onClick={this.addPolicy}>Add Policy</button>
            </div>

            <h3 className="permissionsSectionTitle">Import</h3>
            <div className="inputScreenIntro">
                <p>
                    Reads a Mosquitto <code>acl_file</code> or a <code>dynamic-security.json</code>.
                    Importing is planned first: rules already present are recognised and not
                    duplicated, a rule an existing rule already covers is not added, and an allow
                    that overlaps a deny is a conflict that has to be answered before anything is
                    stored. The order Mosquitto would check the imported rules in is kept, and
                    %u/%c are substituted when a rule is checked, not when it is imported.
                </p>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Format:</label>
                <select className="inputScreenSelect" value={this.state.importFormat}
                        onChange={e => this.setState({importFormat: e.target.value, importPlan: null})}>
                    <option value="acl">Mosquitto ACL file</option>
                    <option value="dynsec">dynamic-security.json</option>
                </select>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">File:</label>
                <FileLoad accept=".acl,.conf,.json,.txt" onLoad={this.importFileLoaded}/>
                {this.state.importName
                    ? <span className="inputScreenHint">{this.state.importName}</span>
                    : null}
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">or paste it:</label>
                <textarea rows={8} style={{width: "600px", fontFamily: "monospace"}}
                          value={this.state.importContent}
                          onChange={e => this.setState({importContent: e.target.value, importPlan: null})}/>
            </div>
            <div className="inputScreenRow">
                <button className="inputScreenSaveButton inputScreenActionButton"
                        disabled={this.state.importBusy} onClick={this.previewImport}>Plan import</button>
            </div>
            {this.renderImportPlan()}
        </>;
    }

    renderTestAccess() {
        return <>
            <div className="inputScreenIntro">
                <p>
                    Answers the question a rule is meant to answer, by the same rule the broker
                    applies: may this account publish or subscribe to this topic? Leave the username
                    empty to test an anonymous client.
                </p>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Username:</label>
                <input value={this.state.testUsername}
                       onChange={e => this.setState({testUsername: e.target.value})}/>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Client id:</label>
                <input value={this.state.testClientId}
                       onChange={e => this.setState({testClientId: e.target.value})}/>
                <span className="inputScreenHint">Needed for rules that use %c.</span>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Action:</label>
                <select className="inputScreenSelect" value={this.state.testAction}
                        onChange={e => this.setState({testAction: e.target.value})}>
                    <option value="subscribe">Subscribe</option>
                    <option value="publish">Publish</option>
                </select>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Topic or filter:</label>
                <input value={this.state.testTopic}
                       onChange={e => this.setState({testTopic: e.target.value})}/>
            </div>
            <div className="inputScreenRow">
                <button className="inputScreenSaveButton inputScreenActionButton"
                        disabled={this.state.testBusy} onClick={this.runTest}>Test access</button>
            </div>
            {this.renderDecision()}
        </>;
    }
}
