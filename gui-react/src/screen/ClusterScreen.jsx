import React from "react";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import BasicScreen from "./BasicScreen";
import "./InputScreen.css";
import PageTitle from "../components/PageTitle";
import DialogWindow from "../components/DialogWindow";
import {confirmationWindow, errorWindow, informationWindow} from "../components/MessageWindow";

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
 * Add, Invite, Remove, and the invitation this node has received - shown as a button, reviewed
 * before it is accepted, because accepting restarts the node onto another storage and another
 * accounts database.
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
        selected: null,     // the selected node, for Invite and Remove
        dialog: null,       // "add" or "invite" while a dialog is open
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
                // here would make the Invite button claim nothing was selected.
                this.setState({cluster: result, error: null});
                return true;
            });
    }

    addNode(values) {
        this.send("add", {node: {name: values.name, host_port: values.host_port}},
                  "Can't add the node");
    }

    removeNode(row) {
        const selected = row || this.state.selected;
        if (!selected) {
            errorWindow("Please select a node to remove.");
            return;
        }
        confirmationWindow("Remove node \"" + selected.name + "\" from the cluster's configuration?")
            .then(confirmed => {
                if (confirmed) {
                    this.send("remove", {node: {node_id: selected.node_id, name: selected.name}},
                              "Can't remove the node");
                }
            });
    }

    inviteNode(values) {
        const selected = this.state.selected;
        if (!selected) {
            errorWindow("Please select a node to invite.");
            return;
        }
        this.send("invite", {node: {node_id: selected.node_id, name: selected.name}, password: values.password},
                  "Can't invite the node");
    }

    /**
     * Accepting applies what the invitation offered and ends the process with the restart code:
     * the node comes back on the cluster's storage and its accounts database.
     * @param {Object} invitation  The held invitation, as the server described it.
     */
    reviewInvitation(invitation) {
        const persistence = invitation.current_persistence === true
            ? "Persistence: on (" + (invitation.current_redis_uri || "no storage") + ")  ->  the cluster's storage: " + invitation.redis_uri
            : "Persistence: off  ->  on, using the cluster's storage: " + invitation.redis_uri;
        const accounts = "Accounts: " + (invitation.current_database_uri || "local") +
                         "  ->  the cluster's database: " + invitation.database_uri;

        confirmationWindow(
            "Accept the invitation from \"" + invitation.inviter_name + "\" (" + invitation.inviter_host_port + ")?\n\n" +
            "The node will restart and this is what changes:\n\n" +
            persistence + "\n" + accounts + "\n" +
            "Cluster: on, and the node takes its place in the cluster.\n\n" +
            "The node's clients disconnect while it restarts. Local persistent sessions, queued " +
            "messages and local accounts are NOT carried over: they stay behind in the previous " +
            "configuration, which is kept as the last-good copy until the node has joined.\n\n" +
            "Accept?",
            "Accept Invitation")
            .then(confirmed => {
                if (!confirmed) {
                    return;
                }
                this.send("accept", {}, "Can't accept the invitation").then(accepted => {
                    if (accepted) {
                        informationWindow("The node is restarting into the cluster. This interface will be " +
                                          "unavailable while it does; reload the page afterwards.");
                    }
                });
            });
    }

    declineInvitation() {
        confirmationWindow("Decline the invitation from this cluster? The inviting node is told.")
            .then(confirmed => {
                if (confirmed) {
                    this.send("decline", {}, "Can't decline the invitation");
                }
            });
    }

    renderInvitation() {
        const invitation = this.state.cluster ? this.state.cluster.invitation : null;
        if (!invitation || !invitation.inviter_name) {
            return null;
        }

        const refused = invitation.reject_reason && invitation.reject_reason.length > 0;

        return <fieldset style={{borderColor: "#b26a00"}}>
            <legend>Invitation</legend>
            <p>
                <b>{invitation.inviter_name}</b> ({invitation.inviter_host_port}) invites this node to its
                cluster, running XMQ {invitation.xmq_version}. Received {invitation.received_at}.
            </p>
            <p>
                The cluster works against the storage <b>{invitation.redis_uri}</b> and keeps its
                accounts in <b>{invitation.database_uri}</b>.
            </p>
            {refused
                ? <p style={{color: "#c62828"}}>
                    This invitation cannot be accepted: {invitation.reject_reason}
                </p>
                : null}
            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" disabled={refused}
                        onClick={() => this.reviewInvitation(invitation)}>
                    Review and Accept&hellip;
                </button>
                <button className="dataTableRemoveButton" onClick={() => this.declineInvitation()}>
                    Decline
                </button>
                <span className="inputScreenHint">
                    Review opens a confirmation naming every change the accept would make.
                </span>
            </div>
        </fieldset>;
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
                <button className="inputScreenSaveButton" disabled={this.state.busy}
                        onClick={() => {
                            if (!this.state.selected) {
                                errorWindow("Please select a node to invite.");
                                return;
                            }
                            this.setState({dialog: "invite"});
                        }}>
                    Invite&hellip;
                </button>
                <span className="inputScreenHint">
                    Add declares a node by its name and its TLS address; nothing is contacted.
                    Invite sends it the invitation, and asks for the cluster account's password.
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

        if (this.state.dialog === "invite") {
            const selected = this.state.selected;
            return <DialogWindow windowTitle={"Invite " + (selected ? selected.name : "")}
                                 fields={[
                                     {headerName: "Cluster password", field: "password", context: {inputType: "password"}}
                                 ]}
                                 values={{password: ""}}
                                 windowWidth={560} windowHeight={260}
                                 onSubmit={(result, values) => {
                                     if (result === "Ok") {
                                         this.inviteNode(values);
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

            {cluster ? this.renderInvitation() : null}

            {cluster && !cluster.on_cluster_database
                ? <p style={{color: "#666", maxWidth: "46em"}}>
                    This node keeps its accounts in a file of its own (SQLite), and a cluster's
                    configuration is kept in a PostgreSQL or MySQL database shared by the nodes.
                    There is nothing to declare here until this node joins a cluster by invitation.
                </p>
                : null}

            {cluster && cluster.on_cluster_database ? this.renderClusterSettings() : null}
            {cluster && cluster.on_cluster_database ? this.renderNodes() : null}

            {this.renderDialogs()}
        </div>;
    }
}