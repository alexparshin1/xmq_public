import React from "react";
import {errorWindow} from "../components/MessageWindow";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import HelpButton from "../components/HelpButton";
import {isValidWhitelist} from "../ipWhitelist";
import "./InputScreen.css";
import PageTitle from "../components/PageTitle";

/**
 * Listeners screen.
 * The fields match the Listener type in xmq.wsdl, that is the "connections.listener"
 * section of the server configuration.
 */
export default class ListenersScreen extends React.Component {

    state = {
        headers: [
            {
                // Carried through the editor so that "modify" can name the listener it changes:
                // the server matches on the id, and rejects a change that doesn't carry one.
                headerName: "Id", field: "id", hide: true,
                context: {hide: true, inputType: 'hidden'}
            },
            {
                headerName: "Name", field: "name", width: 250, cellClass: "ViewCellLeftAligned"
            },
            {
                headerName: "Port", field: "port", type: 'numericColumn', width: 100,
                context: {inputType: 'integer'}
            },
            {
                headerName: "Protocol", field: "protocol", width: 150, cellClass: "ViewCellLeftAligned",
                context: {inputType: 'select', options: ["MQTT", "MQTT+SSL", "MQTT+WS", "MQTT+WSS"]}
            },
            {
                headerName: "Bind IP address", field: "bind_ip", width: 180,
                cellClass: "ViewCellLeftAligned"
            },
            {
                headerName: "IP white list", field: "whitelist", width: 260,
                cellClass: "ViewCellLeftAligned",
                context: {inputType: 'text'}
            },
            {
                headerName: "Threads", field: "threads", type: 'numericColumn', width: 120,
                context: {inputType: 'integer', required: false}
            },
            {
                headerName: "Enabled", field: "enable", width: 110, cellClass: "ViewCellLeftAligned",
                context: {inputType: 'select', options: ["true", "false"]}
            }
        ],
        rows: [],
        dataVersion: 0
    };

    componentDidMount() {
        ControlAPI.asyncMakeAPICall("ListenerControl", {action: "list"})
            .then(data => {
                if (!data || !data.list) {
                    return;
                }
                // The editor's selects work in strings, while the server sends and expects
                // a boolean, so the flag is converted on the way in and on the way out.
                this.setState({
                    rows: data.list.map(listener => ({
                        ...listener,
                        enable: String(listener.enable === true)
                    }))
                });
            });
    }

    render() {
        return <div>
            <PageTitle>Listeners</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Each listener is a port the server accepts MQTT connections on. A disabled
                    listener keeps its settings but does not open its port.
                    <HelpButton helpKey="listener.protocol"/>
                </p>
                <p>
                    Changes apply immediately: the port is reopened as soon as the listener is
                    saved. Thread counts
                    <HelpButton helpKey="listener.threads"/>
                    and bind addresses
                    <HelpButton helpKey="listener.bind_ip"/>
                    are explained by the buttons here, and the IP white list
                    <HelpButton helpKey="listener.whitelist"/>
                    by its own.
                </p>
            </div>

            <DataTable objectName="Listener" paging={false} headers={this.state.headers}
                       rows={this.state.rows}
                       onRowDataChange={this.onRowDataChange}
                       editorWidth={"450px"} editorHeight={"320px"}/>
        </div>;
    }

    onRowDataChange = (action, data) => {
        if ((action === "add" || action === "modify") && !isValidWhitelist(data.whitelist)) {
            errorWindow("The IP white list is not valid.\n\nEnter comma-separated IPv4 addresses, "
                + "each with an optional /prefix, for example: 10.1.1.1/24,10.1.2.123/32,10.1.1.242\n"
                + "Leave it empty to allow any IP address.");
            return;
        }

        const listener = {
            ...data,
            port: parseInt(data.port),
            // Threads are optional in the listener API. Leave an empty field out of the request;
            // the server defaults it to one accept thread.
            ...(data.threads === "" || data.threads == null ? {} : {threads: parseInt(data.threads)}),
            enable: String(data.enable) === "true"
        };

        ControlAPI.asyncMakeAPICall("ListenerControl", {action: action, listener: listener})
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't save the listener: " + error);
                    return;
                }
                this.componentDidMount();
                this.setState({dataVersion: this.state.dataVersion + 1});
            });
    }
}
