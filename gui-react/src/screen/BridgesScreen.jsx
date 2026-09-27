import React from "react";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import HelpButton from "../components/HelpButton";
import BasicScreen from "./BasicScreen";
import "./InputScreen.css";
import PageTitle from "../components/PageTitle";
import {errorWindow} from "../components/MessageWindow";
import PasswordInput from "../components/PasswordInput";

/**
 * Password placeholder. The server never sends a bridge password back, it sends this instead,
 * and accepts it on save to mean "keep the stored password".
 */
const MaskedPassword = "*****";

/**
 * An empty bridge, used when adding one.
 */
const NewBridge = {
    id: 0,
    node_name: "",
    enabled: true,
    host: "",
    port: 1883,
    username: "",
    password: "",
    client_id: "",
    clean_session: false,
    encrypted: false,
    cafile: "",
    certfile: "",
    keyfile: "",
    verify_depth: 0,
    topics: []
};

/**
 * Bridges screen, as a master-detail page: the bridge list on top, and the settings and
 * topics of the selected bridge below. The fields match the Bridge and BridgeTopic types
 * in xmq.wsdl.
 */
export default class BridgesScreen extends BasicScreen {

    state = {
        bridgeHeaders: [
            {headerName: "Name", field: "node_name", cellClass: "ViewCellLeftAligned", width: 150},
            {headerName: "Host", field: "host_port", cellClass: "ViewCellLeftAligned", width: 200},
            {headerName: "Mode", field: "mode", cellClass: "ViewCellLeftAligned", width: 100},
            {headerName: "Topics", field: "topic_count", width: 100},
            {headerName: "Enabled", field: "enabled", cellClass: "ViewCellLeftAligned", width: 100}
        ],
        topicHeaders: [
            {
                headerName: "Id", field: "id", hide: true,
                context: {hide: true, inputType: 'hidden'}
            },
            {
                headerName: "Pattern", field: "pattern", cellClass: "ViewCellLeftAligned", width: 250
            },
            {
                headerName: "Direction", field: "direction", cellClass: "ViewCellLeftAligned", width: 150,
                context: {inputType: 'select', options: ["inout", "in", "out"]}
            },
            {
                headerName: "QoS", field: "qos", width: 100,
                context: {inputType: 'integer'}
            }
        ],
        rows: [],
        // The bridge currently shown in the detail form, in the page's own flat shape.
        selected: null,
        dataVersion: 0
    };

    componentDidMount() {
        ControlAPI.asyncMakeAPICall("BridgeControl", {action: "list"})
            .then(data => {
                if (!data || !data.list) {
                    return;
                }
                // Cluster node connections are carried as bridges too. Clustering isn't part of
                // the interface, so those are left out rather than shown as something editable.
                const bridges = data.list
                                    .filter(bridge => bridge.mode !== "cluster")
                                    .map(bridge => ({
                                        ...bridge,
                                        topic_count: bridge.topics ? bridge.topics.length : 0
                                    }));
                this.setState({rows: bridges, selected: null});
            });
    }

    /**
     * Splits a bridge as the server sends it into the flat shape the detail form edits.
     * @param {Object} bridge    Bridge from BridgeControl
     * @return {Object} the bridge, with host_port and ssl_keys split into separate fields
     */
    toFormValues(bridge) {
        const hostPort = bridge.host_port ? bridge.host_port.split(":") : ["", "1883"];
        const sslKeys = bridge.ssl_keys ? bridge.ssl_keys : {};
        const topics = bridge.topics ? bridge.topics : [];
        return {
            id: bridge.id,
            node_name: bridge.node_name,
            enabled: bridge.enabled,
            host: hostPort[0],
            port: hostPort.length > 1 ? hostPort[1] : 1883,
            mode: bridge.mode ? bridge.mode : "out",
            username: bridge.username ? bridge.username : "",
            password: bridge.password ? bridge.password : "",
            client_id: bridge.client_id ? bridge.client_id : "",
            clean_session: bridge.clean_session === true,
            encrypted: bridge.encrypted === true,
            cafile: sslKeys.cafile ? sslKeys.cafile : "",
            certfile: sslKeys.certfile ? sslKeys.certfile : "",
            keyfile: sslKeys.keyfile ? sslKeys.keyfile : "",
            verify_depth: sslKeys.verify_depth ? sslKeys.verify_depth : 0,
            // The topics table needs a row id to tell an added row from an edited one.
            topics: topics.map((topic, index) => ({
                id: index + 1,
                pattern: topic.pattern,
                direction: topic.direction ? topic.direction : "inout",
                qos: topic.qos === undefined ? 1 : topic.qos
            }))
        };
    }

    /**
     * Builds the Bridge object to send, from the detail form.
     * @return {Object} bridge, in the shape BridgeControl expects
     */
    toBridge() {
        const selected = this.state.selected;
        const bridge = {
            node_name: selected.node_name,
            enabled: selected.enabled,
            host_port: selected.host + ":" + selected.port,
            username: selected.username,
            password: selected.password,
            client_id: selected.client_id,
            clean_session: selected.clean_session,
            encrypted: selected.encrypted,
            mode: selected.mode,
            // Sent without the row ids the topics table added for its own use.
            topics: selected.topics.map(topic => ({
                pattern: topic.pattern,
                direction: topic.direction,
                qos: parseInt(topic.qos)
            }))
        };

        if (selected.id) {
            bridge.id = selected.id;
        }

        if (selected.encrypted) {
            bridge.ssl_keys = {
                cafile: selected.cafile,
                certfile: selected.certfile,
                keyfile: selected.keyfile,
                verify_depth: String(selected.verify_depth)
            };
        }

        return bridge;
    }

    setField(fieldName, value) {
        this.setState({
            selected: {
                ...this.state.selected,
                [fieldName]: value
            }
        });
    }

    /**
     * Applies an Add, Edit, or Remove from the topics table to the selected bridge.
     * Nothing reaches the server until the bridge itself is saved.
     */
    onTopicChange = (action, topic) => {
        const topics = this.state.selected.topics;
        let updated;
        if (action === "add") {
            const nextId = topics.reduce((maxId, row) => Math.max(maxId, row.id), 0) + 1;
            updated = [...topics, {...topic, id: nextId, qos: parseInt(topic.qos)}];
        } else if (action === "modify") {
            updated = topics.map(row => row.id === topic.id
                                        ? {...topic, qos: parseInt(topic.qos)}
                                        : row);
        } else {
            updated = topics.filter(row => row.id !== topic.id);
        }
        this.setField("topics", updated);
    }

    saveBridge() {
        const selected = this.state.selected;
        if (!selected.node_name || !selected.host) {
            errorWindow("A bridge needs a name and a host.");
            return;
        }
        if (selected.topics.length === 0) {
            errorWindow("A bridge with no topics carries no messages. Add at least one topic.");
            return;
        }

        const action = selected.id ? "modify" : "add";
        ControlAPI.asyncMakeAPICall("BridgeControl", {action: action, bridge: this.toBridge()})
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't save the bridge: " + error);
                    return;
                }
                this.componentDidMount();
                this.setState({dataVersion: this.state.dataVersion + 1});
            });
    }

    /**
     * Rebuilds the server's bridge connections from the saved configuration.
     * Saving a bridge only writes the configuration; the connections are built when the bridges
     * are started, so a change does nothing visible until this is pressed (or the server is
     * restarted).
     */
    applyBridges = () => {
        ControlAPI.asyncMakeAPICall("BridgeControl", {action: "apply"})
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't apply the bridges: " + error);
                    return;
                }
                this.componentDidMount();
            });
    }

    onBridgeListChange = (action, bridge) => {
        if (action === "remove") {
            // The whole bridge, not just its id. The server validates the CBridge it is handed
            // against the schema before it looks anything up, so an id on its own is rejected
            // for an empty node_name, host_port and mode - and that rejection arrives in the
            // shape apiError() below exists to cope with. topic_count is this page's own
            // display column and is not part of the type.
            const {topic_count, ...bridgeToRemove} = bridge;
            ControlAPI.asyncMakeAPICall("BridgeControl", {action: "remove", bridge: bridgeToRemove})
                .then(result => {
                    if (result === null) {
                        return;
                    }
                    const error = ControlAPI.apiError(result);
                    if (error) {
                        errorWindow("Can't remove the bridge: " + error);
                        return;
                    }
                    this.componentDidMount();
                });
        }
    }

    renderTextField(label, fieldName, width = "200px", helpKey = null) {
        return <div className="inputScreenRow">
            <label htmlFor={fieldName} className="inputScreenLabel">{label}:</label>
            <input name={fieldName} type="text" style={{width: width}}
                   value={this.state.selected[fieldName]}
                   onChange={(e) => this.setField(fieldName, e.target.value)}/>
            <HelpButton helpKey={helpKey}/>
        </div>
    }

    renderCheckField(label, fieldName, helpKey = null) {
        return <div className="inputScreenRow">
            <label htmlFor={fieldName} className="inputScreenLabel">{label}:</label>
            <input name={fieldName} type="checkbox"
                   checked={this.state.selected[fieldName]}
                   onChange={(e) => this.setField(fieldName, e.target.checked)}/>
            <HelpButton helpKey={helpKey}/>
        </div>
    }

    renderSslKeys() {
        if (!this.state.selected.encrypted) {
            return null;
        }
        return <div>
            {this.renderTextField("CA certificate file", "cafile", "300px")}
            {this.renderTextField("Certificate file", "certfile", "300px")}
            {this.renderTextField("Key file", "keyfile", "300px")}
            <div className="inputScreenRow">
                <label htmlFor="verify_depth" className="inputScreenLabel">Verify depth:</label>
                <input name="verify_depth" type="number" style={{width: '70px'}}
                       value={this.state.selected.verify_depth}
                       onChange={(e) => this.setField("verify_depth", e.target.value)}/>
            </div>
        </div>
    }

    renderBridgeDetail() {
        const selected = this.state.selected;
        if (!selected) {
            return <div className="inputScreenIntro">
                <p>Select a bridge to edit it, or press Add to create one.</p>
            </div>
        }

        return <div>
            <fieldset>
                <legend>{selected.id ? "Bridge: " + selected.node_name : "New Bridge"}</legend>

                {this.renderTextField("Node name", "node_name", "200px")}

                <div className="inputScreenRow">
                    <label htmlFor="host" className="inputScreenLabel">Host:</label>
                    <input name="host" type="text" style={{width: '200px'}}
                           value={selected.host}
                           onChange={(e) => this.setField("host", e.target.value)}/>
                    <label htmlFor="port" className="inputScreenLabel"
                           style={{width: '50px'}}>port:</label>
                    <input name="port" type="number" style={{width: '80px'}}
                           value={selected.port}
                           onChange={(e) => this.setField("port", e.target.value)}/>
                </div>

                <div className="inputScreenRow">
                    <label htmlFor="mode" className="inputScreenLabel">Mode:</label>
                    <select name="mode" className="inputScreenSelect" value={selected.mode}
                            onChange={(e) => this.setField("mode", e.target.value)}>
                        <option value="out">out</option>
                        <option value="in">in</option>
                        <option value="inout">inout</option>
                    </select>
                    <HelpButton helpKey="bridge.mode"/>
                </div>

                {this.renderTextField("Username", "username")}

                <div className="inputScreenRow">
                    <label htmlFor="password" className="inputScreenLabel">Password:</label>
                    <PasswordInput name="password" style={{width: '200px'}}
                                   value={selected.password}
                                   onChange={(e) => this.setField("password", e.target.value)}/>
                    {selected.password === MaskedPassword &&
                        <span className="inputScreenNote">unchanged</span>}
                </div>

                {this.renderTextField("Client ID", "client_id", "200px", "bridge.client_id")}
                {this.renderCheckField("Enabled", "enabled")}
                {this.renderCheckField("Clean session", "clean_session", "bridge.clean_session")}
                {this.renderCheckField("Encrypted", "encrypted", "bridge.encrypted")}
                {this.renderSslKeys()}
            </fieldset>

            <fieldset>
                <legend>Topics <HelpButton helpKey="bridge.topic_pattern"/></legend>
                <DataTable objectName="Topic" headers={this.state.topicHeaders}
                           rows={selected.topics} height={260} paging={false}
                           onRowDataChange={this.onTopicChange}
                           editorWidth={"450px"} editorHeight={"260px"}/>
            </fieldset>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" onClick={() => this.saveBridge()}>
                    Save Bridge
                </button>
            </div>
        </div>
    }

    render() {
        return <div>
            <PageTitle>Bridges</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    A bridge is a client connection this server makes to another MQTT broker,
                    carrying the topics listed for it. The remote broker doesn't have to be XMQ.
                </p>
            </div>

            <DataTable objectName="Bridge" headers={this.state.bridgeHeaders}
                       rows={this.state.rows} height={260} paging={false}
                       buttons={["add", "remove"]}
                       onAdd={() => this.setState({selected: {...NewBridge, mode: "out", topics: []}})}
                       onSelectionChanged={(row) => this.setState({
                           selected: row ? this.toFormValues(row) : null
                       })}
                       onRowDataChange={this.onBridgeListChange}/>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" onClick={this.applyBridges}>
                    Apply
                </button>
                <span className="inputScreenHint">
                    Rebuilds the bridge connections from the saved configuration. Saved changes
                    do not take effect until this is pressed.
                </span>
            </div>

            {this.renderBridgeDetail()}
        </div>;
    }
}
