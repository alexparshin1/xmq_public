import React from "react";
import {errorWindow} from "./MessageWindow";
import {AgGridReact} from 'ag-grid-react'; // React Data Grid Component
import "ag-grid-community/styles/ag-grid.css"; // Mandatory CSS required by the Data Grid
import "ag-grid-community/styles/ag-theme-quartz.css";
import DialogWindow from "./DialogWindow";
import './DataTable.css';

export default class DataTable extends React.Component {
    constructor(props) {
        super(props);
        // The columns are not copied into state. They used to be, once, in this constructor - so a
        // page that filled part of a column in later (the groups an account may be put in, asked
        // of the server after the first render) was showing them to a table that had stopped
        // looking. The grid and the editor read the current props instead.
        this.gridRef = React.createRef();
        this.objectName = props.objectName;
        this.editorTitle = "";
        this.editorWidth = props.editorWidth ? props.editorWidth : "400px";
        this.editorHeight = props.editorHeight ? props.editorHeight : "150px";
        this.editorValues = {};
        this.onRowDataChanged = props.onRowDataChange
    }

    state = {
        showEditor: false
    };

    setData(rows) {
        this.setState({rows: rows})
    }

    render() {
        const pagination = this.props.paging;
        const paginationPageSize = 500;
        const paginationPageSizeSelector = [200, 500, 1000];
        // Which of the Add / Edit / Remove buttons to offer. A master-detail page edits the
        // selected row in a form of its own, so it asks for the list without an Edit button.
        const buttons = this.props.buttons ? this.props.buttons : ["add", "edit", "remove"];
        const height = this.props.height ? this.props.height : 600;

        // The buttons sit in a column beside the grid rather than after it: as siblings of a
        // grid that fills the container's height, they used to overhang its bottom edge and
        // land on top of whatever the page put below the table.
        return <div className="dataTableContainer" style={{height: height}}>
            <div className="ag-theme-quartz dataTableGrid">
                <AgGridReact
                    ref={this.gridRef}
                    rowData={this.props.rows}
                    getRowId={this.props.getRowId}
                    columnDefs={this.props.headers}
                    rowSelection="single"
                    pagination={pagination}
                    paginationPageSize={paginationPageSize}
                    paginationPageSizeSelector={paginationPageSizeSelector}
                    onSelectionChanged={() => this.selectionChanged()}
                />
            </div>
            <div className="dataTableButtons">
                {buttons.includes("add") &&
                    <button className='dataTableAddEditButton' onClick={() => this.add()}>Add</button>}
                {buttons.includes("edit") &&
                    <button className='dataTableAddEditButton' onClick={() => this.edit()}>Edit</button>}
                {buttons.includes("remove") &&
                    <button className='dataTableRemoveButton' onClick={() => this.remove()}>Remove</button>}
            </div>
            {this.state.showEditor &&
                <DialogWindow windowTitle={this.editorTitle} fields={this.props.headers}
                              values={this.editorValues}
                              windowWidth={this.editorWidth} windowHeight={this.editorHeight}
                              onSubmit={(result, values) => {
                                  if (result === "Ok") this.updateSelectedRow(values);
                              }}
                              closeModal={() => this.setState({showEditor: false})}/>}
        </div>
    }

    /**
     * Reports the selected row to the owning screen, for pages that show the selection's
     * detail elsewhere. Does nothing when the page didn't ask to be told.
     */
    selectionChanged() {
        if (this.props.onSelectionChanged) {
            this.props.onSelectionChanged(this.getSelectedRow());
        }
    }

    add() {
        // A master-detail page edits new rows in its own form, so it takes Add over entirely
        // rather than having the row dialog opened here.
        if (this.props.onAdd) {
            this.props.onAdd();
            return;
        }
        this.editorTitle = "Add New " + this.objectName;
        this.editorValues = {};
        this.setState({showEditor: true});
    }

    edit() {
        const rowId = this.getSelectedRowId();
        if (rowId == null) {
            this.requireSelection("edit");
            return;
        }
        this.editorTitle = "Edit " + this.objectName;
        this.editorValues = this.getSelectedRow();
        this.setState({showEditor: true});
    }

    remove() {
        const row = this.getSelectedRow();
        if (row == null) {
            this.requireSelection("remove");
            return;
        }
        if (this.onRowDataChanged) {
            this.onRowDataChanged("remove", row);
        }
    }

    /**
     * Says what the button needed and did not get. A button that does nothing at all reads as a
     * broken button rather than as one waiting for a selection.
     * @param {string} action  What was being attempted, for the message.
     */
    requireSelection(action) {
        const name = this.objectName ? this.objectName.toLowerCase() : "row";
        errorWindow("Please select a " + name + " to " + action + ".");
    }

    getSelectedRowId() {
        const selectedRows = this.gridRef.current.api.getSelectedRows();
        if (selectedRows.length === 1) {
            return selectedRows[0].id;
        }
        return null;
    }

    getSelectedRow() {
        const selectedRows = this.gridRef.current.api.getSelectedRows();
        if (selectedRows.length === 1) {
            return selectedRows[0];
        }
        return null;
    }

    updateSelectedRow(row) {
        let action = row.id ? "modify" : "add";
        if (this.onRowDataChanged) {
            this.onRowDataChanged(action, row);
        }
    }
}
