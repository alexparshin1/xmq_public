import React from "react";
import ExtensionsScreen from "./ExtensionsScreen";
import PageTitle from "../components/PageTitle";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import {groupKey, groupOf} from "../extensionGroups";
import "../components/DialogWindow.css";

/**
 * The list of configurations a user created from one family of extensions. Which family is fixed
 * by the `page` the extensions declare; the same screen serves Data Integrations and
 * Authentication. Buttons operate on the selected row, never on the extension itself.
 */
export default class ConfigurationsScreen extends ExtensionsScreen
{
    constructor(props) {
        super(props);
        // Draft names for keys that belong to a declared family but do not exist yet.
        this.state.newKeyDrafts = {};
    }

    /// The page an extension has to declare to belong here. Data Integrations and Authentication
    /// override this; the Permissions screen embeds this component with `page="permissions"`.
    page() { return this.props.page || ""; }

    pageTitle() { return this.props.title || "Configurations"; }

    pageDescription() { return this.props.description || ""; }

    objectLabel() {
        if (this.props.objectLabel) return this.props.objectLabel;
        return this.page() === "data-integrations" ? "data integration" : "configuration";
    }

    addDialogTitle() {
        if (this.props.addTitle) return this.props.addTitle;
        if (this.page() === "data-integrations") return "Add Data Integration";
        return "Add " + this.pageTitle();
    }

    /// Only data integrations own a persistent queue, so the other pages do not promise one.
    deleteQuestion(label) {
        if (this.page() === "data-integrations") return super.deleteQuestion(label);
        return "Delete the " + this.objectLabel() + " '" + label + "'? This cannot be undone.";
    }

    componentDidMount() {
        super.componentDidMount();
        document.addEventListener("keydown", this.onDialogKey);
    }

    componentWillUnmount() {
        document.removeEventListener("keydown", this.onDialogKey);
    }

    /// The dialog follows the usual modal convention: Escape cancels, Enter accepts. Enter keeps
    /// its normal meaning in a textarea, where it inserts a line rather than saving the form.
    onDialogKey = (event) => {
        if (!this.state.showEdit) return;
        if (event.key === "Escape") {
            event.preventDefault();
            this.closeEdit();
        }
        else if (event.key === "Enter" && event.target.tagName !== "TEXTAREA") {
            event.preventDefault();
            if (!this.state.busy && !this.invalidSettings().length) {
                this.applyEdit();
            }
        }
    }

    headers = [
        {headerName: "Name", field: "name", width: 220, cellClass: "ViewCellLeftAligned"},
        {headerName: "Extension", field: "extension_name", width: 170, cellClass: "ViewCellLeftAligned"},
        {headerName: "State", field: "state", width: 150, cellClass: "ViewCellLeftAligned"},
        {headerName: "Outcome", field: "outcome", width: 360, cellClass: "ViewCellLeftAligned"}
    ];

    configurations() {
        const group = groupKey(this.page());
        return this.state.extensions.filter(extension => groupOf(extension) === group);
    }

    /// The grid takes plain rows, so the derived strings are computed here and the original
    /// description travels with the row for the actions on the right.
    rows() {
        return this.configurations().map(extension => ({
            id: ExtensionsScreen.extensionId(extension),
            extension: extension,
            name: extension.display_name || extension.extension_instance || extension.name,
            extension_name: extension.name,
            // The checkbox in the dialog is the configuration's own flag. The grid says both
            // switches: a configuration can be enabled here and still read "extension disabled"
            // because of the extension-wide switch, or be enabled and loaded but not started until
            // its settings
            // have what they need.
            state: extension.running ? "active"
                   : !extension.extension_enabled ? "extension disabled"
                   : !extension.configuration_enabled ? "disabled"
                   : "enabled",
            outcome: "dropped " + (extension.messages_dropped || 0) +
                     " · rejected " + (extension.schema_rejected || 0) +
                     " · sink " + (extension.sink_failures || 0) +
                     " · queue " + (extension.queue_failures || 0)
        }));
    }

    addTypes() {
        const seen = new Set();
        return this.state.extensions.filter(extension => {
            // An authorizer is broker-wide: its rules are one ordered set, and a second settings
            // set would have no defined precedence. Named sets are for exporters, not permissions.
            if (extension.sink || extension.authorizer ||
                groupOf(extension) !== groupKey(this.page()) || seen.has(extension.name)) return false;
            seen.add(extension.name);
            return true;
        });
    }

    /// The buttons sit in the column beside the grid, as on Listeners. Every one of them works on
    /// the row the operator selected; Add is the exception, since it has no row to select yet.
    renderActions() {
        const selected = this.selected();
        const adds = this.addTypes();
        const singleAuthorizer = !adds.length && this.configurations().some(extension => extension.authorizer);
        return <React.Fragment>
            {singleAuthorizer
                ? <span className="inputScreenHint">One broker-wide configuration</span>
                : <button className="dataTableAddEditButton" disabled={this.state.busy || !adds.length}
                          onClick={this.openAdd}>Add</button>}
            <button className="dataTableAddEditButton" disabled={this.state.busy || !selected}
                    onClick={() => this.openEdit(selected)}>Edit</button>
            {/* With no Add there is no way to get this configuration back, so Delete would be a
                one-way trap. The broker-wide switch for it lives on Installed Extensions. */}
            {singleAuthorizer ? null
                : <button className="dataTableRemoveButton" disabled={this.state.busy || !selected}
                          onClick={() => this.deleteIntegration(selected)}>Delete</button>}
        </React.Fragment>;
    }

    onSelectionChanged = (row) => {
        this.setState({selected: row ? row.id : null});
    }

    openEdit = (extension) => {
        if (!extension) return;
        const id = ExtensionsScreen.extensionId(extension);
        this.setState({selected: id, edited: {}, preset: "", presetResult: null,
                       editEnabled: !!extension.configuration_enabled, showEdit: true});
    }

    /// A family declaration such as `group.*` is not a key, but it says which keys are allowed.
    /// Adding `operators` here creates `group.operators` as a field the operator can fill in.
    addFamilyKey = family => {
        const draft = String((this.state.newKeyDrafts || {})[family.name] || "").trim();
        if (!draft) return;
        const name = family.name.slice(0, -1) + draft;
        if (Object.keys(this.state.edited).includes(name)) return;
        this.setState(state => ({
            edited: {...state.edited, [name]: ""},
            newKeyDrafts: {...state.newKeyDrafts, [family.name]: ""}
        }));
    }

    /// Ok on a dialog whose fields were not touched changes only the flag. Sending every field back
    /// on a flag-only edit would rewrite stored settings with whatever the form shows - a blank
    /// field is not the same as an explicit request to clear a key.
    applyEdit = () => {
        if (Object.keys(this.state.edited).length !== 0) {
            this.apply(() => this.saveConfigEnabled());
            return;
        }
        this.setState({busy: true});
        this.saveConfigEnabled().then(data => {
            this.setState({busy: false});
            if (!ExtensionsScreen.succeeded(data)) return;
            this.closeEdit();
            this.refresh(data.message);
        });
    }

    saveConfigEnabled = () => {
        const extension = this.selected();
        if (!extension) return Promise.resolve();
        return ControlAPI.asyncMakeAPICall("ExtensionControl", {
            action: "set_configuration_enabled",
            name: extension.name,
            extension_instance: extension.extension_instance || "",
            // A required extension has no off switch; never send one even if the dialog was first
            // opened while the flag was already false in the file.
            enabled: extension.required ? true : this.state.editEnabled
        });
    }

    renderEditDialog() {
        if (!this.state.showEdit) return null;
        const extension = this.selected();
        if (!extension) return null;
        const required = !!extension.required;
        // An empty database_uri here is only noise: this extension takes the address from the
        // broker's authentication section, and an empty string is not a setting anybody made.
        const userDatabase = extension.name === "user-database";
        const declared = (extension.settings || []).filter(setting => !setting.name.endsWith(".*"));
        const families = (extension.settings || []).filter(setting => setting.name.endsWith(".*"));
        const added = Object.keys(this.state.edited)
            .filter(name => !declared.some(setting => setting.name === name))
            .map(name => ({name, label: name, type: "string", value: this.state.edited[name], declared: true}));
        const formExtension = {
            ...extension,
            settings: [...declared, ...added].filter(setting =>
                !userDatabase || setting.name !== "database_uri" || !!this.valueOf(setting))
        };
        return <div className="modalBackground">
            <div className="modalContainer configurationEditor" role="dialog" aria-label="Edit configuration">
                <div className="title">Edit {extension.display_name || extension.name}</div>
                <div className="body">
                    <div className="extensionForm">
                        <span/>
                        <label htmlFor="configurationEnabled">
                            <input id="configurationEnabled" type="checkbox"
                                   disabled={required}
                                   checked={!!this.state.editEnabled}
                                   onChange={event => this.setState({editEnabled: event.target.checked})}/>
                            Enabled
                            {required
                                ? <span className="inputScreenHint"> — required extension</span>
                                : null}
                        </label>
                    </div>
                    <React.Fragment>
                            {userDatabase
                                ? <p className="extensionNote">
                                    The account database URI comes from Initial Setup. The credentials below
                                    are this extension's account in that database; leave them empty for SQLite.
                                  </p>
                                : null}
                            {this.presetsOf(extension).length ? this.renderPresets(extension) : null}
                            {families.length ? <div className="extensionForm">
                                <span/>
                                <div className="extensionListEditor">
                                    {families.map(family =>
                                        <div className="extensionListRow" key={family.name}>
                                            <input type="text"
                                                   placeholder={"new " + family.name.slice(0, -1) + "name"}
                                                   value={(this.state.newKeyDrafts || {})[family.name] || ""}
                                                   onChange={event => this.setState(state => ({newKeyDrafts: {
                                                       ...state.newKeyDrafts,
                                                       [family.name]: event.target.value
                                                   }}))}
                                                   onKeyDown={event => {
                                                       if (event.key === "Enter") {
                                                           event.preventDefault();
                                                           event.stopPropagation();
                                                           this.addFamilyKey(family);
                                                       }
                                                   }}/>
                                            <button type="button" className="extensionListButton"
                                                    disabled={!String((this.state.newKeyDrafts || {})[family.name] || "").trim()}
                                                    onClick={() => this.addFamilyKey(family)}>Add</button>
                                        </div>)}
                                </div>
                            </div> : null}
                            {this.renderFields(formExtension)}
                            {this.invalidSettings().length
                                ? <p role="alert" className="extensionNote extensionValueError">
                                    Fix the settings marked in red before saving.
                                  </p>
                                : null}
                    </React.Fragment>
                </div>
                <hr className="configurationEditorDivider"/>
                <div className="footer">
                    <button className="modalOkButton"
                            disabled={this.state.busy || this.invalidSettings().length}
                            onClick={this.applyEdit}>Ok</button>
                    <button className="modalCancelButton" onClick={this.closeEdit}>Cancel</button>
                </div>
            </div>
        </div>;
    }

    render() {
        return <div>
            {/* Embedded in another page (Permissions), the host page already has its title and
                introduction; repeating them inside a tab would show two page headings. */}
            {this.props.embedded ? null : <>
                <PageTitle>{this.pageTitle()}</PageTitle>
                <div className="inputScreenIntro">
                    <p>{this.pageDescription()}</p>
                </div>
            </>}
            <DataTable objectName="Configuration" headers={this.headers} rows={this.rows()}
                       getRowId={params => params.data.id} paging={false}
                       buttons={[]} sidePanel={this.renderActions()}
                       onSelectionChanged={this.onSelectionChanged}/>
            <p className="inputScreenHint">
                Actions apply to the selected row. A configuration runs only when it is enabled
                here and its extension is enabled on the Installed Extensions page.
            </p>
            {this.renderAddDialog()}
            {this.renderEditDialog()}
        </div>;
    }
}
