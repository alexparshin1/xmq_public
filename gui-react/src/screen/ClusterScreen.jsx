import React from "react";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import BasicScreen from "./BasicScreen";
import "./InputScreen.css";
import "./ClusterScreen.css";
import PageTitle from "../components/PageTitle";
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
 * A URI with its password dropped for display: this page only shows the addresses, and a
 * password shown is a password copied. It is never sent back anywhere.
 * @param {string} uri  Storage or database URI as the server reports it.
 * @return {string} URI without the password.
 */
function displayUri(uri) {
    return (uri || "").replace(/^([a-z][a-z0-9+.-]*:\/\/[^:/?#@]*):[^/]*@/i, "$1@");
}

/**
 * Cluster Configuration.
 *
 * The cluster is configured here, not by editing a file on each node: the nodes with their states,
 * Start cluster, Join cluster, Leave cluster and Remove. Start cluster forms a cluster on a
 * Standalone node's configured Redis storage and accounts database; Join cluster explicitly confirms
 * a restart onto the shared Redis storage for a node already using the same accounts database.
 *
 * The mode itself - Standalone or Cluster - is changed by those actions; the Dashboard only shows
 * what it is.
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
        selected: null,     // every node action targets this row
        busy: false
    };

    componentDidMount() {
        this.refresh();
        this.refreshTimer = setInterval(() => this.refresh(), 3000);
    }

    componentWillUnmount() {
        clearInterval(this.refreshTimer);
        this.unmounted = true;
    }

    updateCluster(result) {
        if (this.unmounted) return;
        this.setState(state => ({
            cluster: result, error: null,
            selected: state.selected
                ? (result.nodes || []).find(node => node.node_id === state.selected.node_id) || null
                : null
        }));
    }

    /**
     * Reads the whole picture: ClusterControl answers every action with it, so one call refreshes
     * the page after any change.
     */
    refresh() {
        ControlAPI.asyncMakeAPICall("ClusterControl", {action: "get"}, true)
            .then(result => {
                if (this.unmounted) return;
                if (result === null) {
                    this.setState({error: "The XMQ interface did not answer the request."});
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    this.setState({error: error, cluster: null});
                    return;
                }
                this.updateCluster(result);
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
                this.updateCluster(result);
                return true;
            });
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
            (force ? "For an unrecoverable Offline member, first ensure the failed server is powered off or isolated from Redis. " +
                "Removal is refused while it holds a live lease. Its old identity will be blocked from rejoining; " +
                "persisted sessions and queued messages remain available for recovery by the cluster. Continue?"
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
        const starting = this.state.cluster && this.state.cluster.cluster_state === "empty";
        confirmationWindow(starting
            ? 'Start a cluster on node "' + selected.name + '" using its configured Redis storage and shared accounts database? ' +
              "MQTT will restart; its clients will reconnect."
            : "Join node \"" + selected.name + "\" to this cluster?\n\n" +
            "The selected node will use this cluster's Redis storage and restart MQTT. Its clients " +
            "will disconnect briefly; the web interface stays available. Local standalone sessions and queued messages are " +
            "not moved into the cluster. The shared accounts database stays the same.")
            .then(confirmed => {
                if (confirmed) {
                    this.send(starting ? "start" : "join", {node: {node_id: selected.node_id, name: selected.name}},
                              starting ? "Can't start the cluster" : "Can't join the node to the cluster");
                }
            });
    }

    leaveCluster() {
        const selected = this.state.selected;
        if (!selected) return;
        confirmationWindow('Leave the cluster and restart MQTT on node "' + selected.name + '" as Standalone? ' +
            "Its configuration in this database will be preserved. The web interface stays available.")
            .then(confirmed => confirmed && this.send("disable", {
                node: {node_id: selected.node_id, name: selected.name}
            }, "Can't leave the cluster"));
    }

    retireConfiguration() {
        const selected = this.state.selected;
        if (!selected) return;
        confirmationWindow('Retire configuration for node "' + selected.name + '" in this database? MQTT will stop. ' +
            "The server will not start on this database until it is explicitly reconfigured. " +
            "This acknowledges that the configuration may subsequently be removed.")
            .then(confirmed => confirmed && this.send("retire", {
                node: {node_id: selected.node_id, name: selected.name}
            }, "Can't retire the configuration"));
    }

    renderClusterSettings() {
        const cluster = this.state.cluster;
        return <fieldset>
            <legend>This Node</legend>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Mode:</label>
                <span>{cluster.enabled ? "Cluster" : "Standalone"}</span>
                <span className="inputScreenHint">
                    Use the actions below to start or join a cluster. Restore an existing Offline member on its Dashboard.
                </span>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Storage:</label>
                <span>{displayUri(cluster.redis_uri) || "—"}</span>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Accounts database:</label>
                <span>{displayUri(cluster.database_uri) || "—"}</span>
            </div>
            <div className="inputScreenRow">
                <label className="inputScreenLabel">Coordinator:</label>
                <span>{cluster.coordinator || "none"}</span>
            </div>
        </fieldset>;
    }

    renderNodeActions() {
        const {cluster, selected, busy} = this.state;
        const state = selected && selected.state;
        const member = ["Clustered", "Offline", "Unknown", "Joining"].includes(state);
        const active = selected && !["Retired", "Removed", "Not Clustered"].includes(state);
        const buttonClass = "inputScreenSaveButton inputScreenActionButton";
        const starting = cluster.cluster_state === "empty";
        return <div className="clusterNodeActions">
            <div className="clusterSelectedNode" aria-live="polite">
                {selected ? <>Selected node: <strong>{selected.name}</strong></> : "Select a node"}
            </div>
            <button className={buttonClass}
                    disabled={busy || (!starting && ["offline", "unknown"].includes(cluster.cluster_state)) ||
                        !selected || state !== "Standalone"}
                    onClick={() => this.joinNode()}>{starting ? "Start cluster" : "Join cluster"}&hellip;</button>
            <button className={buttonClass} disabled={busy || !active || !member}
                    onClick={() => this.leaveCluster()}>Leave cluster&hellip;</button>
            <button className={buttonClass} disabled={busy || !active || member}
                    onClick={() => this.retireConfiguration()}>Retire configuration&hellip;</button>
            <button className={buttonClass} disabled={busy || !selected}
                    onClick={() => this.removeNode()}>Remove node&hellip;</button>
            <button className={buttonClass} disabled={busy || !selected}
                    onClick={() => this.removeNode(null, true)}>Force remove node&hellip;</button>
        </div>;
    }

    renderNodes() {
        const cluster = this.state.cluster;
        return <fieldset>
            <legend>Cluster Nodes</legend>
            {this.renderClusterGuidance()}
            <DataTable objectName="Node" headers={this.state.headers}
                       rows={cluster.nodes || []} height={320} paging={false}
                       getRowId={params => params.data.node_id}
                       buttons={[]} sidePanel={this.renderNodeActions()}
                       onSelectionChanged={row => this.setState({selected: row})}/>
            <p className="inputScreenHint">
                Actions apply to the selected node, including when it is on another host.
                Join cluster admits a node that already shares this database.
            </p>
        </fieldset>;
    }

    renderClusterGuidance() {
        const cluster = this.state.cluster;
        if (cluster.cluster_state === "empty") return <p className="inputScreenHint">
            Redis has no cluster members. Select a Standalone node and press Start cluster.
            Then open that node's Cluster page to join the other nodes.
        </p>;
        if (cluster.cluster_state === "offline") return <div role="status" className="inputScreenHint">
            The cluster still exists, but all its members are Offline:
            {" " + (cluster.cluster_members || []).map(node => node.name).join(", ") + ". "}
            Start one of these members. On its Cluster page, press Start cluster if it is Standalone,
            or start MQTT if it is stopped. Then use that member's Cluster page to join other nodes.
            If a member cannot be restored after hardware or OS failure, isolate the failed server,
            select its row and use Force remove node. When no members remain, Start cluster becomes available.
        </div>;
        if (cluster.cluster_state === "unknown") return <p role="status" className="inputScreenHint">
            Cluster membership could not be read from Redis. Restore storage connectivity before
            starting a cluster or force removing a member.
        </p>;
        const offline = (cluster.cluster_members || []).filter(node => node.state === "Offline");
        if (offline.length) return <div role="status" className="inputScreenHint">
            Offline members: {offline.map(node => node.name).join(", ")}. Start the affected member;
            on its Cluster page, press Start cluster if it is Standalone, or start MQTT if it is stopped.
            If hardware or OS failure prevents recovery, isolate that server and use Force remove node
            on its selected row. The remaining Clustered members can continue serving the cluster.
            {!cluster.enabled && " Open a Clustered member's Cluster page to join other nodes."}
        </div>;
        if (!cluster.enabled && cluster.cluster_state === "active") return <p className="inputScreenHint">
            This node is Standalone. Open the Cluster page on a Clustered member to join nodes to its cluster.
        </p>;
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

        </div>;
    }
}
