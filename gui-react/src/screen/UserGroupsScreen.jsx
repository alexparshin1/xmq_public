import React from "react";
import {errorWindow} from "../components/MessageWindow";
import DataTable from "../components/DataTable";
import ControlAPI from "../ControlAPI";
import "./InputScreen.css";
import PageTitle from "../components/PageTitle";

/**
 * Groups screen.
 *
 * A group is a name and nothing else for now - what a group is allowed to do arrives with the
 * permissions work. It exists already because membership does: accounts are put in groups on the
 * Users page, and they have to be put in something that was created here first.
 */
export default class UserGroupsScreen extends React.Component {

    state = {
        headers: [
            {
                // The server matches on the name; the id is carried so a row can be edited.
                headerName: "Id", field: "id", hide: true,
                context: {hide: true, inputType: 'hidden'}
            },
            {headerName: "Group", field: "name", width: 300, cellClass: "ViewCellLeftAligned"}
        ],
        rows: []
    }

    componentDidMount() {
        ControlAPI.asyncMakeAPICall("UserGroupControl", {action: "list"})
            .then(data => {
                if (!data) {
                    return;
                }
                this.setState({rows: data.list ? data.list : []});
            });
    }

    render() {
        return <div>
            <PageTitle>Groups</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Groups are what accounts are put in on the Users page. A group needs to exist
                    here before an account can belong to it: the server refuses a group it does not
                    know rather than creating one, so that a name typed wrongly is reported instead
                    of quietly becoming a group of its own.
                </p>
                <p>
                    Removing a group takes everybody's membership of it with it. The accounts
                    themselves are not touched.
                </p>
            </div>

            <DataTable objectName="Group" headers={this.state.headers}
                       rows={this.state.rows}
                       getRowId={params => String(params.data.id)}
                       onRowDataChange={this.onRowDataChange}
                       editorWidth={"420px"} editorHeight={"200px"}/>
        </div>;
    }

    onRowDataChange = (action, data) => {
        // The server knows add and remove only: a group is a name, and renaming one would be
        // indistinguishable from removing it and adding another - which is what it would do to
        // everybody's membership, so it is not offered.
        if (action === "modify") {
            errorWindow("A group cannot be renamed. Remove it and add the new name, "
                  + "remembering that its members go with it.");
            return;
        }

        ControlAPI.asyncMakeAPICall("UserGroupControl", {action: action, group: data})
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't save the group: " + error);
                    return;
                }
                this.componentDidMount();
            });
    }
}
