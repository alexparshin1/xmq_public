import React from "react";
import {Link} from "react-router-dom";
import {errorWindow} from "../components/MessageWindow";
import ExtensionsScreen from "./ExtensionsScreen";
import PageTitle from "../components/PageTitle";
import DataTable from "../components/DataTable";
import {groupKey, groupLabel, groupOf, groupPath} from "../extensionGroups";

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
        {headerName: "Group", field: "group", width: 170, cellClass: "ViewCellLeftAligned"},
        {headerName: "Description", field: "description", width: 460, cellClass: "ViewCellLeftAligned"}
    ];

    /// A sink has no settings set of its own: it is chosen inside an export's configuration. It
    /// cannot be enabled or disabled here, so listing it would only offer a switch that does not
    /// work. The exporters that use sinks are listed normally.
    installed() {
        const seen = new Set();
        return this.state.extensions.filter(extension => {
            if (extension.sink || seen.has(extension.name)) return false;
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
            // There is no Start/Stop on this page: running is a property of a configuration, not
            // of an extension. The two switches here decide whether configurations may run at all.
            state: (extension.extension_enabled ? "enabled" : "disabled") +
                   (extension.required ? " (required)" : ""),
            role: this.capabilitiesOf(extension),
            // The config's group wins; without one the library's declared page is the group, the
            // same fallback the menu uses. Only an extension with neither has no group.
            group: groupLabel(extension) || "—",
            description: extension.description || "—"
        }));
    }

    /// Required settings with no value, for the hint beside the buttons. Enabling still loads an
    /// extension without them: only starting it needs an endpoint or another such setting.
    missingSettings(extension) {
        return (extension.settings || []).filter(setting => setting.required &&
            !String(setting.value === undefined || setting.value === null ? "" : setting.value).trim())
            .map(setting => setting.name);
    }

    /// Where those settings are edited, when the extension has a settings page at all.
    settingsLink(extension) {
        const key = groupOf(extension);
        if (!key) return null;
        return <Link to={groupPath(key)}>the {groupKey(key) === "permissions" ? "Permissions" :
            (extension.group || key.replace(/-/g, " ").replace(/\b\w/g, c => c.toUpperCase()))} page</Link>;
    }

    /// A required extension may be enabled after it was switched off in the file, but it must not
    /// be disabled from here: without it the broker cannot serve MQTT. Say why when it is tried,
    /// rather than leaving a grey button with no explanation.
    disableSelected = () => {
        const selected = this.selected();
        if (!selected) return;
        if (selected.required) {
            errorWindow("This extension is required and can't be disabled.");
            return;
        }
        this.toggleExtension(selected);
    }

    /// Enable and Disable act on the selected extension, as the Listeners buttons do. Enable loads
    /// the library and keeps it available; a configuration starts when its settings are ready.
    renderActions() {
        const selected = this.selected();
        return <React.Fragment>
            <button className="dataTableAddEditButton"
                    disabled={this.state.busy || !selected || selected.extension_enabled}
                    onClick={() => this.toggleExtension(selected)}>Enable</button>
            <button className="dataTableRemoveButton"
                    disabled={this.state.busy || !selected || !selected.extension_enabled}
                    onClick={this.disableSelected}>Disable</button>
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
                Actions apply to the selected extension. Enable loads it; a configuration starts
                when its required settings are present.
            </p>
            {(() => {
                const selected = this.selected();
                if (!selected) return null;
                const missing = this.missingSettings(selected);
                if (!missing.length) return null;
                return <p className="inputScreenHint">
                    <b>{selected.name}</b> needs {missing.join(", ")} before a configuration can start
                    {this.settingsLink(selected) ? <> — set it on {this.settingsLink(selected)}</> : null}.
                </p>;
            })()}
        </div>;
    }
}
