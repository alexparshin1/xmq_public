import React from "react";
import ExtensionsScreen from "./ExtensionsScreen";
import PageTitle from "../components/PageTitle";
import DataTable from "../components/DataTable";

/**
 * Broker-wide control: the extensions this installation has, not the configurations a user made
 * from them. Enabling or disabling a row applies to every configuration of that extension; each
 * configuration keeps its own flag for when the extension is available again.
 */
export default class InstalledExtensionsScreen extends ExtensionsScreen
{
    headers = [
        {headerName: "Extension", field: "name", width: 200, cellClass: "ViewCellLeftAligned"},
        {headerName: "Version", field: "version", width: 100, cellClass: "ViewCellLeftAligned"},
        {headerName: "State", field: "state", width: 150, cellClass: "ViewCellLeftAligned"},
        {headerName: "Role", field: "role", width: 170, cellClass: "ViewCellLeftAligned"},
        {headerName: "What it is for", field: "description", width: 460, cellClass: "ViewCellLeftAligned"}
    ];

    installed() {
        const seen = new Set();
        return this.state.extensions.filter(extension => {
            if (seen.has(extension.name)) return false;
            seen.add(extension.name);
            return true;
        });
    }

    rows() {
        return this.installed().map(extension => ({
            id: extension.name,
            extension: extension,
            name: extension.name,
            version: extension.version || "—",
            state: (extension.running ? "running" : "stopped") + (extension.required ? " (required)" : ""),
            role: this.capabilitiesOf(extension),
            description: extension.description || "—"
        }));
    }

    /// Enable and Disable act on the selected extension, as the Listeners buttons do. A required
    /// extension has no switch: stopping it would take MQTT down without saying so.
    renderActions() {
        const selected = this.selected();
        return <React.Fragment>
            <button className="dataTableAddEditButton"
                    disabled={this.state.busy || !selected || selected.required || selected.extension_enabled}
                    onClick={() => this.toggleExtension(selected)}>Enable</button>
            <button className="dataTableRemoveButton"
                    disabled={this.state.busy || !selected || selected.required || !selected.extension_enabled}
                    onClick={() => this.toggleExtension(selected)}>Disable</button>
        </React.Fragment>;
    }

    onSelectionChanged = (row) => {
        this.setState({selected: row ? row.id : null});
    }

    render() {
        return <div>
            <PageTitle>Installed Extensions</PageTitle>
            <div className="inputScreenIntro">
                <p>
                    An extension must be enabled here before any of its configurations can run.
                    Disabling it stops every configuration that uses it without losing their own
                    enabled state.
                </p>
            </div>
            <DataTable objectName="Extension" headers={this.headers} rows={this.rows()}
                       getRowId={params => params.data.id} paging={false}
                       buttons={[]} sidePanel={this.renderActions()}
                       onSelectionChanged={this.onSelectionChanged}/>
            <p className="inputScreenHint">
                Actions apply to the selected extension.
            </p>
        </div>;
    }
}
