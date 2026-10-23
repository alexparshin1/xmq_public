import React from "react";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import BasicScreen from "./BasicScreen";
import "./InputScreen.css";
import PageTitle from "../components/PageTitle";
import DialogWindow from "../components/DialogWindow";
import {confirmationWindow, errorWindow} from "../components/MessageWindow";

/**
 * The state of a node, coloured the way it should be read: green while the cluster carries its
 * traffic, amber for a member that is down or cut off, red for one that was refused or failed.
 * @param {string} state  State as the server reports it.
 * @return {string} colour for the cell.
 */
function stateColour(state) {
    switch (state) {
        case "Clustered":
            return "#1b7f3b";
        case "Offline":
        case "Expired":
            return "#b26a00";
        case "Failed":
        case "Refused":
            return "#c62828";
        default:
            return "#666";
    }
}

/**
 * Cluster Configuration.
 *
 * The cluster is declared here, not by editing a file on each node: the nodes with their states,
 * Add, Join cluster and Remove. Join cluster explicitly confirms a restart onto shared Redis
 * storage for a node already using the same accounts database.
 *
 * The mode itself - Standalone or Cluster - is switched on the Dashboard; this page shows what
 * the mode means and what the cluster holds.
 */
export default class ClusterScreen extends BasicScreen {

    state = {
        headers: [
            {headerName: "Name", field: "name", cellClass: "ViewCellLeftAligned", width: 180},
            {headerName: "Address", field: "host_port", cellClass: "ViewCellLeftAligned", width: 200},
            {
                headerName: "State", field: "state", cellClass: "ViewCellLeftAligned", width: 140,
                cellStyle: (params) => ({color: stateColour(params.value), fontWeight: "bold"})
            },
            {headerName: "Coordinator", field: "is_coordinator", cellClass: "ViewCellLeftAligned", width: 120},
            {headerName: "Links", field: "links", width: 80},
            {headerName: "Last error", field: "last_error", cellClass: "ViewCellLeftAligned", width: 340}
        ],
        cluster: null,      // last ClusterControl "get" reply
        error: null,        // why the last read failed, shown in place of the picture
        selected: null,     // the selected node, for Join and Remove
        dialog: null,       // "add" while the declaration dialog is open
        busy: false
    };

    componentDidMount() {
        this.refresh();
    }

    /**
     * Reads the whole picture: ClusterControl answers every action with it, so one call refreshes
     * the page after any change.
     */
    refresh() {
        ControlAPI.asyncMakeAPICall("ClusterControl", {action: "get"}, true)
            .then(result => {
                if (result === null) {
                    this.setState({error: "The XMQ interface did not answer the request."});
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    this.setState({error: error, cluster: null});
                    return;
                }
                this.setState({cluster: result, error: null});
            });
    }

    /**
     * Sends one action and folds the answer in: on success the reply carries the new picture,
     * which is what the page then shows.
     * @param {string} action   ClusterControl action.
     * @param {Object} payload  The rest of the request (node, password, ...).
     * @param {string} failure  What to say when the server refuses.
     * @return {Promise<boolean>} whether the action was accepted.
     */
    send(action, payload, failure) {
        this.setState({busy: true});
        return ControlAPI.asyncMakeAPICall("ClusterControl", {action, ...payload})
            .then(result => {
                this.setState({busy: false});
                if (result === null) {
                    return false;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow(failure + ": " + error);
                    return false;
                }
                // The selection is kept: the grid still shows the row selected, and clearing it
                // here would make the Join cluster button claim nothing was selected.
                this.setState({cluster: result, error: null});
                return true;
            });
    }

    addNode(values) {
        this.send("add", {node: {name: values.name, host_port: values.host_port}},
                  "Can't add the node");
    }

    removeNode(row, force = false) {
        const selected = row || this.state.selected;
        if (!selected) {
            errorWindow("Please select a node to remove.");
            return;
        }
        confirmationWindow((force ? "FORCE REMOVE node \"" : "Remove node \"") + selected.name +
            "\" and its configuration from this database? This is not Leave cluster. " +
            "A server still using this configuration will not restart on this database. " +
            (force ? "Its UUID or Offline status cannot prove it no longer uses this database. Continue anyway?"
                   : "Retire the configuration on the server first if it is still in use."))
            .then(confirmed => {
                if (confirmed) {
                    this.send("remove", {node: {node_id: selected.node_id, name: selected.name}, force},
                              "Can't remove the node");
                }
            });
    }

    joinNode() {
        const selected = this.state.selected;
        if (!selected) {
            errorWindow("Please select a Standalone node to join.");
            return;
        }
        confirmationWindow("Join node \"" + selected.name + "\" to this cluster?\n\n" +
            "The selected node will use this cluster's Redis storage and restart. Its clients " +
            "will disconnect during restart. Local standalone sessions and queued messages are " +
            "not moved into the cluster. The shared accounts database stays the same.")
            .then(confirmed => {
                if (confirmed) {
                    this.send("join", {node: {node_id: selected.node_id, name: selected.name}},
                              "Can't join the node to the cluster");
                }
            });
    }

    leaveCluster() {
        confirmationWindow("Leave the cluster and restart this node as Standalone? " +
            "Its configuration in this database will be preserved.")
            .then(confirmed => confirmed && this.send("disable", {}, "Can't leave the cluster"));
    }

    retireConfiguration() {
        confirmationWindow("Retire this server's configuration in this database? MQTT will stop. " +
            "The server will not start on this database until it is explicitly reconfigured. " +
            "This acknowledges that the configuration may subsequently be removed.")
            .then(confirmed => confirmed && this.send("retire", {}, "Can't retire the configuration"));
    }

    renderClusterSettings() {
        const cluster = this.state.cluster;
        return <fieldset>
            <legend>This Node</legend>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Mode:</label>
                <span>{cluster.enabled ? "Cluster" : "Standalone"}</span>
                <span className="inputScreenHint">
                    The mode is switched on the Dashboard.
                </span>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Storage:</label>
                <span>{cluster.redis_uri || "—"}</span>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Accounts database:</label>
                <span>{cluster.database_uri || "—"}</span>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Coordinator:</label>
                <span>{cluster.coordinator || "none"}</span>
            </div>
        </fieldset>;
    }

    renderNodes() {
        const cluster = this.state.cluster;
        return <fieldset>
            <legend>Cluster Nodes</legend>
            <DataTable objectName="Node" headers={this.state.headers}
                       rows={cluster.nodes ? cluster.nodes : []} height={260} paging={false}
                       buttons={["add", "remove"]}
                       onAdd={() => this.setState({dialog: "add"})}
                       onSelectionChanged={(row) => this.setState({selected: row})}
                       onRowDataChange={(action, row) => {
                           if (action === "remove") {
                               this.removeNode(row);
                           }
                       }}/>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton inputScreenActionButton"
                        disabled={this.state.busy || !cluster.enabled} onClick={() => this.leaveCluster()}>
                    Leave cluster
                </button>
                <button className="inputScreenSaveButton inputScreenActionButton"
                        disabled={this.state.busy || cluster.enabled} onClick={() => this.retireConfiguration()}>
                    Retire this configuration
                </button>
                <button className="inputScreenSaveButton inputScreenActionButton"
                        disabled={this.state.busy || !this.state.selected}
                        onClick={() => this.removeNode(null, true)}>Force remove node&hellip;</button>
                <button className="inputScreenSaveButton inputScreenActionButton"
                        disabled={this.state.busy || !cluster.enabled || !this.state.selected ||
                                  this.state.selected.is_this_node || this.state.selected.state !== "Standalone"}
                        onClick={() => this.joinNode()}>
                    Join cluster&hellip;
                </button>
                <span className="inputScreenHint">
                    Add declares a node by its name and its TLS address; nothing is contacted.
                    Join cluster checks the selected server and restarts it into this cluster.
                </span>
            </div>
        </fieldset>;
    }

    renderDialogs() {
        if (this.state.dialog === "add") {
            return <DialogWindow windowTitle="Add Node"
                                 fields={[
                                     {headerName: "Name", field: "name"},
                                     {headerName: "Address", field: "host_port"}
                                 ]}
                                 values={{name: "", host_port: ""}}
                                 windowWidth={520} windowHeight={240}
                                 onSubmit={(result, values) => {
                                     if (result === "Ok") {
                                         this.addNode(values);
                                     }
                                 }}
                                 closeModal={() => this.setState({dialog: null})}/>;
        }

        return null;
    }

    render() {
        const {cluster, error} = this.state;

        return <div className="InputScreen">
            <PageTitle>Cluster Configuration</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    A cluster is a set of XMQ nodes that share one storage (Redis) and one accounts
                    database (PostgreSQL or MySQL), so that a client connecting to any node sees the
                    same sessions, the same accounts and the same retained messages.
                </p>
            </div>

            {error
                ? <p style={{color: "#c62828"}}>{error}</p>
                : null}

            {cluster && !cluster.on_cluster_database
                ? <p style={{color: "#666", maxWidth: "46em"}}>
                    This node keeps its accounts in a file of its own (SQLite), and a cluster's
                    configuration is kept in a PostgreSQL or MySQL database shared by the nodes.
                    Configure the shared accounts database in Initial Setup before using Join cluster.
                </p>
                : null}

            {cluster && cluster.on_cluster_database ? this.renderClusterSettings() : null}
            {cluster && cluster.on_cluster_database ? this.renderNodes() : null}

            {this.renderDialogs()}
        </div>;
    }
}
