import React from "react";
import {errorWindow, informationWindow} from "../components/MessageWindow";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import FileLoad from "../components/FileLoad";
import PageTitle from "../components/PageTitle";
import "./InputScreen.css";

/**
 * Permissions screen: topic rules, a way to try them, and an import.
 *
 * A rule names a group - `user:<name>`, an account group, or `anonymous` - a direction (publish or
 * subscribe), and a topic filter. A group that has any rule is an allow-list: a topic no rule in it
 * covers is refused, and a deny always wins over an allow. "read" from Mosquitto is subscribe here,
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
                // A combo box, not a select: the existing groups are offered, and `user:<name>`,
                // `anonymous` or a group that has just been created elsewhere can still be typed.
                headerName: "Group", field: "group", width: 200, cellClass: "ViewCellLeftAligned",
                context: {inputType: "combobox", options: []}
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
        testTopic: "",
        testAction: "subscribe",
        testResult: null,
        testBusy: false,

        importFormat: "acl",
        importName: "",
        importContent: "",
        importPlan: null,
        importResolution: "reject",
        importBusy: false
    }

    componentDidMount() {
        this.readGroups();
        this.readRules();
    }

    /**
     * Fills the Group field's list with the groups the server knows.
     *
     * A refusal is not reported here: the field is a combo box and still accepts a typed value, so
     * a page whose group list did not arrive is less useful but not broken. Reporting it as well
     * would put an error in front of a page that has already loaded.
     */
    readGroups() {
        return ControlAPI.asyncMakeAPICall("UserGroupControl", {action: "list"})
            .then(data => {
                if (!data || ControlAPI.apiError(data) || !data.list) {
                    return;
                }
                const names = data.list.map(group => group.name).filter(name => name);
                // Not a group, but one of the rule targets: offered so it need not be remembered.
                if (!names.includes("anonymous")) {
                    names.push("anonymous");
                }
                this.setState(state => {
                    const current = state.headers.find(header => header.field === "group");
                    const known = current && current.context ? current.context.options : [];
                    if (known.length === names.length && known.every((name, at) => name === names[at])) {
                        return null;
                    }
                    return {
                        headers: state.headers.map(header => header.field === "group"
                            ? {...header, context: {...header.context, options: names}}
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
                        access: rule.deny ? "deny" : "allow"
                    }))
                });
                return true;
            });
    }

    onRowDataChange = (action, data) => {
        const rule = {
            group: String(data.group || "").trim(),
            action: data.action,
            deny: data.access === "deny",
            filter: String(data.filter || "").trim(),
            source: data.source || "manual",
            source_ref: data.source_ref || ""
        };

        if (action === "remove") {
            ControlAPI.asyncMakeAPICall("PermissionsControl", {action: "remove", rule_id: data.id})
                .then(result => {
                    if (PermissionsScreen.succeeded(result)) {
                        this.readRules();
                    }
                });
            return;
        }

        if (!rule.group) {
            errorWindow("A rule needs a group: user:<name>, an account group, or anonymous.");
            return;
        }
        if (!rule.filter) {
            errorWindow("A rule needs a topic filter.");
            return;
        }

        // Editing is add-then-remove rather than remove-then-add: if the server refuses the new
        // rule the old one is still there, and when nothing actually changed the add updates the
        // same row and the old id is the one to keep.
        ControlAPI.asyncMakeAPICall("PermissionsControl", {action: "add", rule: rule})
            .then(result => {
                if (!PermissionsScreen.succeeded(result)) {
                    return;
                }
                if (action !== "modify") {
                    this.readRules();
                    return;
                }
                const saved = (result.rules ? result.rules : []).find(candidate =>
                    candidate.group === rule.group && candidate.action === rule.action &&
                    candidate.deny === rule.deny && candidate.filter === rule.filter);
                if (!saved || saved.id === data.id) {
                    this.readRules();
                    return;
                }
                return ControlAPI.asyncMakeAPICall("PermissionsControl",
                    {action: "remove", rule_id: data.id})
                    .then(removed => {
                        if (PermissionsScreen.succeeded(removed)) {
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
        const describe = rule => rule.group + " " + (rule.deny ? "deny " : "allow ") +
            rule.action + " " + rule.filter;
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
        return <div>
            <PageTitle>Permissions</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    These rules decide which topics an account may publish and subscribe to. A
                    group is <code>user:&lt;name&gt;</code> for one account, the name of a group
                    from the Groups page, or <code>anonymous</code> for clients that connect
                    without a username.
                </p>
                <p>
                    A group that has any rule here is an allow-list: a topic no rule in it covers is
                    refused, and a deny always wins over an allow. "Subscribe" is also permission to
                    receive on that filter. Per-listener rules are not available yet, and a
                    subscription is matched as a filter rather than as the literal topic some brokers
                    distinguish - both are the same for ordinary clients.
                </p>
            </div>

            <DataTable objectName="Rule" headers={this.state.headers}
                       rows={this.state.rows}
                       getRowId={params => String(params.data.id)}
                       onRowDataChange={this.onRowDataChange}
                       editorWidth={"470px"} editorHeight={"330px"}/>

            <PageTitle>Test access</PageTitle>
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

            <PageTitle>Import</PageTitle>
            <div className="inputScreenIntro">
                <p>
                    Reads a Mosquitto <code>acl_file</code> or a <code>dynamic-security.json</code>.
                    Importing is planned first: rules already present are recognised and not
                    duplicated, a rule an existing rule already covers is not added, and an allow
                    that overlaps a deny is a conflict that has to be answered before anything is
                    stored. Dynamic Security priorities are flattened, so a deny wins here where
                    Mosquitto would take the first matching rule; the plan says so.
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
        </div>;
    }
}
