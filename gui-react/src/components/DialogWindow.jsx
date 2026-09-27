import React from "react";
import './DialogWindow.css';
import PasswordInput from "./PasswordInput";

export default class DialogWindow extends React.Component {
    constructor(props) {
        super(props);
        this.windowTitle = props.windowTitle;
        this.state.fields = props.fields;
        // A copy: editing must not alter the grid row until Ok is pressed.
        this.state.values = {...props.values};
        this.windowRef = React.createRef();
        this.objectName = props.objectName;

        this.state.windowWidth = props.windowWidth !== undefined ? props.windowWidth : 500;
        this.state.windowHeight = props.windowHeight !== undefined ? props.windowHeight : 300;

        // Set the default value for selects, but only where the row has no value yet:
        // overwriting would silently reset the field of every row being edited.
        for (let field of this.state.fields) {
            const context = field.hasOwnProperty("context") ? field.context : field;
            if (context.inputType === "select"
                && this.state.values[field.field] === undefined) {
                this.state.values[field.field] = context.options[0];
            }
            if (context.inputType === "multiselect"
                && !Array.isArray(this.state.values[field.field])) {
                // Empty, and deliberately not the first option: a new account belongs to no group
                // until somebody says otherwise.
                this.state.values[field.field] = [];
            }
        }
    }

    state = {
        values: [],
        fields: [],
        windowWidth: 500,
        windowHeight: 300
    };

    makeInputWidget(field) {
        let context = field;
        if (field.hasOwnProperty("context")) {
            context = field.context;
        }
        if (context.inputType === "multiselect") {
            // Several of a known set - group membership, at the time of writing. A table with a
            // box against every one of them, rather than a list to pick from: this way the ones an
            // account is not in are on screen too, so the question "what else could it be in" is
            // answered by looking rather than by opening something. Typing names is not offered at
            // all - the server refuses a group it does not know, and being refused for a typo is a
            // poor way to find out what the groups are called.
            const chosen = Array.isArray(this.state.values[field.field])
                ? this.state.values[field.field] : [];

            const toggle = (option, wanted) => {
                const next = wanted
                    ? [...chosen, option]
                    : chosen.filter(name => name !== option);
                this.previewValueChange(field, next);
            };

            return <div className="modalTableBox">
                <table className="modalTable">
                    <tbody>
                    {context.options.length === 0 &&
                        <tr>
                            <td colSpan={2} className="modalTableEmpty">Nothing to choose from yet</td>
                        </tr>}
                    {context.options.map(option =>
                        <tr key={option}>
                            <td className="modalTableTick">
                                <input type="checkbox" id={field.field + "-" + option}
                                       checked={chosen.includes(option)}
                                       onChange={(e) => toggle(option, e.target.checked)}/>
                            </td>
                            <td>
                                <label htmlFor={field.field + "-" + option}>{option}</label>
                            </td>
                        </tr>)}
                    </tbody>
                </table>
            </div>
        }

        if (context.inputType === "select") {
            return <select name={field.field} value={this.state.values[field.field]}
                           onChange={(e) => this.previewValueChange(field, e.target.value)}>
                {context.options.map(option => <option key={option} value={option}>{option}</option>)}
            </select>
        }

        if (context.inputType === "password") {
            // Its own widget rather than an input type: the field carries a button that shows what
            // has been typed, and a password set from a dialog is one nobody gets to check twice.
            return <PasswordInput name={field.field}
                                  value={this.state.values[field.field]}
                                  onChange={(e) => this.previewValueChange(field, e.target.value)}
                                  style={{width: this.inputWidth(field)}}
                                  required/>
        }

        let inputType = context.inputType;
        switch (context.inputType) {
            case "integer":
                inputType = "number";
                break;
            default:
                break;
        }

        return <input type={inputType}
                      name={field.field}
                      value={this.state.values[field.field]}
                      onChange={(e) => this.previewValueChange(field, e.target.value)}
                      style={{width: this.inputWidth(field)}}
                      required/>
    }


    render() {
        // wrapping container with theme and size
        return <div key={this.windowRef} ref={this.windowRef} className="modalBackground">
            <div key={this.windowRef + "-container"} className="modalContainer"
                 style={{width: this.state.windowWidth, height: this.state.windowHeight}}>
                <div key="modalTitle" className="title">{this.windowTitle}</div>
                <form className="modalContainerRow">
                    {
                        this.state.fields.map(field => <div key={field.field}>
                            {!field.hasOwnProperty("context") || field.context["hide"] !== true
                                ?
                                <label className="modalContainerLabel" htmlFor={field.field}>{field.headerName}:</label>
                                : <label/>
                            }
                            {this.makeInputWidget(field)}
                        </div>)
                    }
                </form>
                <div className="modalFooterSpace"/>
                <div className="modalFooter">
                    <button className='modalOkButton'
                            onClick={() => this.onSubmit("Ok", this.state.values)}>Ok
                    </button>
                    <button className='modalCancelButton'
                            onClick={() => this.onSubmit("Cancel", this.state.values)}>Cancel
                    </button>
                </div>
            </div>
        </div>
    }

    onSubmit(result, values) {
        this.props.onSubmit(result, values);
        this.props.closeModal();
    }

    inputWidth(field) {
        switch (field.inputType) {
            case "integer":
                return 70;
            case "float":
                return 150;
            default:
                return 250;
        }
    }

    previewValueChange(field, newValue) {
        switch (field.inputType) {
            case "integer":
                newValue = parseInt(newValue);
                break;
            case "float":
                newValue = parseFloat(newValue);
                break;
            default:
                break;
        }
        this.setState({
            values: {
                ...this.state.values,
                [field.field]: newValue
            }
        });
    }
}
