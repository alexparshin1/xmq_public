import React from "react";
import {errorWindow, informationWindow} from "../components/MessageWindow";
import DataTable from "../components/DataTable";
import {subjectLabel} from "../components/DialogWindow";
import ControlAPI from "../ControlAPI";
import FileLoad from "../components/FileLoad";
import PageTitle from "../components/PageTitle";
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
 * Permissions screen: topic rules, a way to try them, and an import.
 *
 * A rule names a group - `user:<name>`, an account group, or `anonymous` - a direction (publish or
 * subscribe), and a topic filter. Rules are checked in order and the first match decides; a client
 * with rules but no match is denied, as in Mosquitto. "read" from Mosquitto is subscribe here,
 * because there is no separate permission for receiving what one is subscribed to.
 *
 * The import is a plan first and a write second. Nothing is stored while a conflict - an allow and
 * a deny that overlap - is unresolved, and a second import of the same file changes nothing.
 */
export default class PermissionsScreen extends React.Component {

    state = {
        headers: [
            {
                // The server removes by id, so it has to survive the editor.
                headerName: "Id", field: "id", hide: true,
                context: {hide: true, inputType: "hidden"}
            },
            {
                // The existing groups and accounts are offered, and `user:<name>`, `anonymous` or a
                // group created elsewhere can still be typed. One row is one subject; adding
                // several in the editor writes one rule for each.
                headerName: "Applies to", field: "group", width: 220, cellClass: "ViewCellLeftAligned",
                // A group has no marker of its own in the stored name; the column adds one so the
                // row reads the same way the editor does.
                valueFormatter: params => params.data && params.data.pattern
                    ? "all clients" : subjectLabel(params.value),
                context: {inputType: "subjects", options: {groups: [], users: []}}
            },
            {
                // A pattern rule is stored under '*' and keeps its %u/%c tokens; an edit has to
                // carry the flag through, or the rule would become a rule for a group named '*'.
                headerName: "Pattern", field: "pattern", hide: true,
                context: {hide: true, inputType: "hidden"}
            },
            {
                headerName: "Action", field: "action", width: 120, cellClass: "ViewCellLeftAligned",
                context: {inputType: "select", options: ["publish", "subscribe"]}
            },
            {
                // "deny" as the editor shows it; the server takes a boolean, converted on the way out.
                headerName: "Rule", field: "access", width: 100, cellClass: "ViewCellLeftAligned",
                context: {inputType: "select", options: ["allow", "deny"]}
            },
            {headerName: "Topic filter", field: "filter", width: 280, cellClass: "ViewCellLeftAligned"},
            {
                // Carried through an edit so a rule that came from an import keeps saying so.
                headerName: "Source", field: "source", hide: true,
                context: {hide: true, inputType: "hidden"}
            },
            {
                headerName: "Source reference", field: "source_ref", hide: true,
                context: {hide: true, inputType: "hidden"}
            }
        ],
        rows: [],

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
     * Fills the editor's lists with the groups and accounts the server knows.
     *
     * A refusal is not reported here: both boxes are combo boxes and still accept a typed value, so
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
            // Not a group, but one of the rule targets: offered so it need not be remembered.
            if (!groupNames.includes("anonymous")) {
                groupNames.push("anonymous");
            }
            const userNames = users && !ControlAPI.apiError(users) && users.list
                ? users.list.map(user => user.username).filter(name => name) : [];
            if (!groupNames.length && !userNames.length) {
                return;
            }
            this.setState(state => {
                const current = state.headers.find(header => header.field === "group");
                const known = current && current.context ? current.context.options : {};
                const same = (first, second) => first.length === second.length &&
                    first.every((name, at) => name === second[at]);
                if (same(known.groups || [], groupNames) && same(known.users || [], userNames)) {
                    return null;
                }
                return {
                    headers: state.headers.map(header => header.field === "group"
                        ? {...header, context: {...header.context,
                            options: {groups: groupNames, users: userNames}}}
                        : header)
                };
            });
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
                // The editor's selects work in strings, while the server sends and expects a
                // boolean, so the flag is converted on the way in and back on the way out.
                this.setState({
                    rows: (data.rules ? data.rules : []).map(rule => ({
                        ...rule,
                        group: rule.group || "",
                        filter: rule.filter || "",
                        access: rule.deny ? "deny" : "allow",
                        pattern: rule.pattern === true
                    }))
                });
                return true;
            });
    }

    /**
     * What stops a rule from being saved, or null when nothing does.
     *
     * Called by the dialog before it closes, so an invalid topic stays on screen to be corrected
     * instead of being sent and refused after the dialog has gone. The same test runs again below,
     * because a caller that bypasses the dialog should not be able to store what it would refuse.
     */
    validateRule = values => {
        const subjects = Array.isArray(values.group)
            ? values.group : [values.group].filter(subject => subject);
        if (!subjects.length) {
            return "A rule needs at least one user or group.";
        }
        const filter = String(values.filter || "").trim();
        if (!filter) {
            return "A rule needs a topic filter.";
        }
        if (!validTopicFilter(filter)) {
            return "'" + filter + "' is not a valid topic filter: levels cannot be empty, "
                + "+ and # must each be a whole level, and # must be the last.";
        }
        return null;
    }

    onRowDataChange = (action, data) => {
        if (action === "remove") {
            ControlAPI.asyncMakeAPICall("PermissionsControl", {action: "remove", rule_id: data.id})
                .then(result => {
                    if (PermissionsScreen.succeeded(result)) {
                        this.readRules();
                    }
                });
            return;
        }

        // The editor works on a list of subjects, while a stored rule names one. A row that was
        // not edited still carries its subject as a string.
        const subjects = [...new Set(Array.isArray(data.group) ? data.group : [data.group])]
            .map(subject => String(subject || "").trim())
            .filter(subject => subject);
        const rule = {
            action: data.action,
            deny: data.access === "deny",
            filter: String(data.filter || "").trim(),
            source: data.source || "manual",
            source_ref: data.source_ref || ""
        };

        const problem = this.validateRule({group: subjects, filter: rule.filter});
        if (problem) {
            errorWindow(problem);
            return;
        }

        const original = action === "modify"
            ? this.state.rows.find(row => row.id === data.id) : null;
        this.saveRule(subjects, rule, original, data.id);
    }

    /**
     * Stores one rule per subject, then drops the rule that was edited if it is not among them.
     *
     * Adding first is what makes an edit safe: a refused new rule leaves the old one in place, and
     * an unchanged one is updated in place, so its id is still the one to keep.
     */
    saveRule(subjects, rule, original, editedId) {
        const savedBySubject = {};
        const addNext = async index => {
            if (index === subjects.length) {
                if (!original || !editedId || savedBySubject[original.group] === editedId) {
                    this.readRules();
                    return;
                }
                const removed = await ControlAPI.asyncMakeAPICall("PermissionsControl",
                    {action: "remove", rule_id: editedId});
                if (PermissionsScreen.succeeded(removed)) {
                    this.readRules();
                }
                return;
            }
            const subject = subjects[index];
            const result = await ControlAPI.asyncMakeAPICall("PermissionsControl",
                {action: "add", rule: {...rule, group: subject, pattern: subject === "*"}});
            if (!PermissionsScreen.succeeded(result)) {
                return;
            }
            const saved = (result.rules ? result.rules : []).find(candidate =>
                candidate.group === subject && candidate.action === rule.action &&
                candidate.deny === rule.deny && candidate.filter === rule.filter &&
                candidate.pattern === (subject === "*"));
            savedBySubject[subject] = saved ? saved.id : null;
            return addNext(index + 1);
        };
        return addNext(0);
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
                // A conflict has to be answered; keeping the existing rule is the safe answer and
                // the one that leaves the broker's behaviour unchanged.
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
        // The extension is a hint, not a decision: a file called .acl that holds JSON is still the
        // operator's file, and guessing wrong only costs a refused parse that they can see.
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
                rather than one long page because the table alone fills the screen, and the test
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
        </div>;
    }

    renderRules() {
        return <>
            <div className="inputScreenIntro">
                <p>
                    These rules decide which topics an account may publish and subscribe to. A
                    rule applies to <code>user:&lt;name&gt;</code> for one account,
                    <code>group:&lt;name&gt;</code> for a group from the Groups page, or
                    <code>anonymous</code> for clients that connect without a username. The
                    <code>group:</code> prefix is how the page shows a group so it cannot be
                    mistaken for a username; the stored rule keeps the bare name.
                </p>
                <p>
                    Rules are checked in order and the first match decides; a client that has rules
                    but none covering the topic is denied, as in Mosquitto. A rule added here is
                    checked before imported ones, and a manual deny before a manual allow.
                    "Subscribe" is also permission to receive on that filter. Per-listener rules are
                    not available yet, and a subscription is matched as a filter rather than as the
                    literal topic some brokers distinguish - both are the same for ordinary clients.
                </p>
            </div>

            <DataTable objectName="Rule" headers={this.state.headers}
                       rows={this.state.rows}
                       getRowId={params => String(params.data.id)}
                       onRowDataChange={this.onRowDataChange}
                       validate={this.validateRule}
                       editorWidth={"580px"} editorHeight={"430px"}/>

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
