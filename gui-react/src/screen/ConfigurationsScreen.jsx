import React from "react";
import ExtensionsScreen from "./ExtensionsScreen";
import PageTitle from "../components/PageTitle";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import "../components/DialogWindow.css";

/**
 * The list of configurations a user created from one family of extensions. Which family is fixed
 * by the `page` the extensions declare; the same screen serves Data Integrations and
 * Authentication. Buttons operate on the selected row, never on the extension itself.
 */
export default class ConfigurationsScreen extends ExtensionsScreen
{
    pageTitle() { return "Configurations"; }

    pageDescription() { return ""; }

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
                this.apply(() => this.saveConfigEnabled());
            }
        }
    }

    headers = [
        {headerName: "Name", field: "name", width: 220, cellClass: "ViewCellLeftAligned"},
        {headerName: "Extension", field: "extension_name", width: 170, cellClass: "ViewCellLeftAligned"},
        {headerName: "State", field: "state", width: 110, cellClass: "ViewCellLeftAligned"},
        {headerName: "Outcome", field: "outcome", width: 360, cellClass: "ViewCellLeftAligned"}
    ];

    configurations() {
        return this.state.extensions.filter(extension => extension.page === this.page());
    }

    /// The grid takes plain rows, so the derived strings are computed here and the original
    /// description travels with the row for the actions on the right.
    rows() {
        return this.configurations().map(extension => ({
            id: ExtensionsScreen.extensionId(extension),
            extension: extension,
            name: extension.display_name || extension.extension_instance || extension.name,
            extension_name: extension.name,
            state: extension.running ? "active" : extension.extension_enabled ? "disabled" : "blocked",
            outcome: "dropped " + (extension.messages_dropped || 0) +
                     " · rejected " + (extension.schema_rejected || 0) +
                     " · sink " + (extension.sink_failures || 0) +
                     " · queue " + (extension.queue_failures || 0)
        }));
    }

    addTypes() {
        const seen = new Set();
        return this.state.extensions.filter(extension => {
            if (extension.sink || extension.page !== this.page() || seen.has(extension.name)) return false;
            seen.add(extension.name);
            return true;
        });
    }

    /// The buttons sit in the column beside the grid, as on Listeners. Every one of them works on
    /// the row the operator selected; Add is the exception, since it has no row to select yet.
    renderActions() {
        const selected = this.selected();
        return <React.Fragment>
            <button className="dataTableAddEditButton" disabled={this.state.busy || !this.addTypes().length}
                    onClick={this.openAdd}>Add</button>
            <button className="dataTableAddEditButton" disabled={this.state.busy || !selected}
                    onClick={() => this.openEdit(selected)}>Edit</button>
            <button className="dataTableRemoveButton" disabled={this.state.busy || !selected}
                    onClick={() => this.deleteIntegration(selected)}>Delete</button>
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

    closeEdit = () => this.setState({showEdit: false, edited: {}, presetResult: null});

    saveConfigEnabled = () => {
        const extension = this.selected();
        if (!extension) return Promise.resolve();
        return ControlAPI.asyncMakeAPICall("ExtensionControl", {
            action: "set_configuration_enabled",
            name: extension.name,
            extension_instance: extension.extension_instance || "",
            enabled: this.state.editEnabled
        });
    }

    renderEditDialog() {
        if (!this.state.showEdit) return null;
        const extension = this.selected();
        if (!extension) return null;
        return <div className="modalBackground">
            <div className="modalContainer configurationEditor" role="dialog" aria-label="Edit configuration">
                <div className="title">Edit {extension.display_name || extension.name}</div>
                <div className="body">
                    <div className="extensionForm">
                        <span/>
                        <label htmlFor="configurationEnabled">
                            <input id="configurationEnabled" type="checkbox"
                                   checked={!!this.state.editEnabled}
                                   onChange={event => this.setState({editEnabled: event.target.checked})}/>
                            Enabled
                        </label>
                    </div>
                    {this.presetsOf(extension).length ? this.renderPresets(extension) : null}
                    {this.renderFields(extension)}
                    {this.invalidSettings().length
                        ? <p role="alert" className="extensionNote extensionValueError">
                            Fix the settings marked in red before saving.
                          </p>
                        : null}
                </div>
                <hr className="configurationEditorDivider"/>
                <div className="footer">
                    <button className="modalOkButton" disabled={this.state.busy || this.invalidSettings().length}
                            onClick={() => this.apply(() => this.saveConfigEnabled())}>Ok</button>
                    <button className="modalCancelButton" onClick={this.closeEdit}>Cancel</button>
                </div>
            </div>
        </div>;
    }

    render() {
        return <div>
            <PageTitle>{this.pageTitle()}</PageTitle>
            <div className="inputScreenIntro">
                <p>{this.pageDescription()}</p>
            </div>
            <DataTable objectName="Configuration" headers={this.headers} rows={this.rows()}
                       getRowId={params => params.data.id} paging={false}
                       buttons={[]} sidePanel={this.renderActions()}
                       onSelectionChanged={this.onSelectionChanged}/>
            <p className="inputScreenHint">
                Actions apply to the selected row. An integration runs only when its extension is
                enabled here and enabled on the Installed Extensions page.
            </p>
            {this.renderAddDialog()}
            {this.renderEditDialog()}
        </div>;
    }
}
