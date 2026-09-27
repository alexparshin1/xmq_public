import React from "react";
import {errorWindow} from "../components/MessageWindow";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import HelpButton from "../components/HelpButton";
import "./InputScreen.css";
import PageTitle from "../components/PageTitle";

/**
 * Users screen.
 * The fields match the User type in xmq.wsdl, that is the "authentication" section of the
 * server configuration.
 */
export default class UsersScreen extends React.Component {

    state = {
        headers: [
            {
                // The server matches "modify" on the id, so it has to survive the editor.
                headerName: "Id", field: "id", hide: true,
                context: {hide: true, inputType: 'hidden'}
            },
            {headerName: "Username", field: "username", width: 180, cellClass: "ViewCellLeftAligned"},
            {
                headerName: "Password", field: "password", width: 140, cellClass: "ViewCellLeftAligned",
                context: {inputType: "password"}
            },
            {
                headerName: "Enabled", field: "is_enabled", width: 100, cellClass: "ViewCellLeftAligned",
                context: {inputType: 'select', options: ["true", "false"]}
            },
            {
                // The whole membership, shown and sent together with the account. The options are
                // the groups the server knows, filled in once they have been asked for.
                headerName: "Groups", field: "groups", width: 250, cellClass: "ViewCellLeftAligned",
                valueFormatter: params => Array.isArray(params.value) ? params.value.join(", ") : "",
                context: {inputType: 'multiselect', options: []}
            }
        ],
        rows: [],
        allow_anonymous: false,
        dataVersion: 0
    }

    componentDidMount() {
        // The groups first, so that the editor has something to offer when a row is opened. A
        // server whose accounts are not in a database has none, and the column then offers
        // nothing - which is the honest showing of a broker that cannot have groups yet.
            ControlAPI.asyncMakeAPICall("UserGroupControl", {action: "list"})
            .then(data => {
                if (ControlAPI.apiError(data) || !data || !data.list) {
                    // A refusal, or a refresh that did not arrive, says nothing about what the
                    // groups are, and the ones already on screen are still the last thing the
                    // server actually said. Replacing them with an empty list is how a save could
                    // leave the editor offering nothing to choose from: the answer to this request
                    // arrives after the dialog has been opened again, and wipes what was there.
                    return;
                }

                const names = data.list.map(group => group.name);
                this.setState(state => {
                    // Replaced only when they differ. Handing the table a new set of columns on
                    // every refresh makes it rebuild them, and it does that while the rows are
                    // being replaced underneath - which is a race worth not having at all, since
                    // the groups rarely change and the columns then need not be touched.
                    const current = state.headers.find(header => header.field === "groups");
                    const known = current && current.context ? current.context.options : [];
                    if (known.length === names.length && known.every((name, at) => name === names[at])) {
                        return null;
                    }
                    return {
                        headers: state.headers.map(header => header.field === "groups"
                            ? {...header, context: {...header.context, options: names}}
                            : header)
                    };
                });
            });

        ControlAPI.asyncMakeAPICall("UserControl", {action: "list"})
            .then(data => {
                if (ControlAPI.apiError(data) || !data || !data.list) {
                    // Same rule as for the groups: a refused answer is not an answer. This one can
                    // arrive half-filled - the accounts gathered and their groups not - and taking
                    // it at face value puts accounts on screen that appear to be in no group.
                    return;
                }
                // The editor's selects work in strings, while the server sends and expects
                // booleans, so the flags are converted on the way in and on the way out.
                this.setState({
                    rows: data.list.map(user => ({
                        ...user,
                        is_enabled: String(user.is_enabled === true),
                        // Absent rather than empty when an account belongs to nothing, and the
                        // editor and the formatter both want a list either way.
                        groups: Array.isArray(user.groups) ? user.groups : []
                    })),
                    allow_anonymous: data.allow_anonymous === true
                });
            });
    }

    setAllowAnonymous(allowAnonymous) {
        this.setState({allow_anonymous: allowAnonymous});
        ControlAPI.asyncMakeAPICall("UserControl",
                                    {action: "modify", allow_anonymous: allowAnonymous})
            .then(result => {
                if (result === null) {
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't change anonymous access: " + error);
                    // Put the checkbox back to what the server still holds.
                    this.setState({allow_anonymous: !allowAnonymous});
                }
            });
    }

    render() {
        return <div>
            <PageTitle>Users</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    These accounts are used both by MQTT clients connecting to the server and for
                    signing in to this interface. An administrator may change the configuration;
                    a non-administrator can connect but not administer.
                </p>
                <p>
                    Passwords are shown as ***** and stay unchanged unless typed over. The admin
                    account cannot be deleted or demoted.
                </p>
                <p>
                    Groups are chosen from those the server knows; saving an account sets its
                    membership to exactly what is selected. Groups themselves are created on the
                    Groups page.
                </p>
                <p>
                    An account administers this server by being in the Administrators group; there
                    is no separate flag for it. Every account is in Default, which is what an
                    ordinary MQTT client needs.
                </p>
            </div>

            <div className="inputScreenRow">
                <label htmlFor="allow_anonymous" className="inputScreenLabel">Allow anonymous:</label>
                <input id="allow_anonymous" name="allow_anonymous" type="checkbox"
                       checked={this.state.allow_anonymous}
                       onChange={(e) => this.setAllowAnonymous(e.target.checked)}/>
                <HelpButton helpKey="authentication.allow_anonymous"/>
            </div>

            <DataTable objectName="User" headers={this.state.headers}
                       rows={this.state.rows}
                       getRowId={params => String(params.data.id)}
                       onRowDataChange={this.onRowDataChange}
                       editorWidth={"450px"} editorHeight={"420px"}/>
        </div>;
    }

    onRowDataChange = (action, data) => {
        const user = {
            ...data,
            is_enabled: String(data.is_enabled) === "true"
        };

        ControlAPI.asyncMakeAPICall("UserControl", {action: action, user: user})
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    // The server's own words when it is the password it objected to: it names
                    // everything the password lacks, all at once, and repeating that here in
                    // different words is how the two come to disagree.
                    errorWindow(/password/i.test(error) ? error : "Can't save the user: " + error);
                    return;
                }
                this.componentDidMount();
                this.setState({dataVersion: this.state.dataVersion + 1});
            });
    }
}
