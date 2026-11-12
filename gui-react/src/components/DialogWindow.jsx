import React from "react";
import './DialogWindow.css';
import PasswordInput from "./PasswordInput";
// The rule table's buttons are the model for this dialog's; importing its sheet keeps them the
// same even on a page where the table happens to be absent.
import "./DataTable.css";

/**
 * How a rule subject is shown: `user:<name>` and `anonymous` already say what they are, a group
 * does not. The prefix is for the eye only - the stored rule and the import keep the bare name.
 */
export function subjectLabel(subject) {
    const text = String(subject ?? "");
    if (text === "*") {
        return "all clients";
    }
    if (!text || text === "anonymous" || text.startsWith("user:") || text.startsWith("group:")) {
        return text;
    }
    return "group:" + text;
}

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
            if (context.inputType === "subjects"
                && !Array.isArray(this.state.values[field.field])) {
                // A row carries one subject, and the editor turns it into a list of them.
                const current = this.state.values[field.field];
                this.state.values[field.field] = current ? [String(current)] : [];
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
        windowHeight: 300,
        // Shown beside the footer when a caller-supplied validation refuses the Ok: the dialog
        // stays open so the value can be corrected.
        error: "",
        // What has been typed into the Add Group / Add User boxes but not added yet.
        drafts: {},
        // Which add box is open for a subjects field, and which entry is selected for Remove.
        pickers: {},
        subjectSelection: {},
        pickerErrors: {}
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

        if (context.inputType === "subjects") {
            // A rule applies to one subject, but a new rule is often the same rule for several of
            // them. The list states the subjects the save will turn into separate rules; the
            // buttons beside it act on that list, so no button can be read as belonging to a row.
            const chosen = Array.isArray(this.state.values[field.field])
                ? this.state.values[field.field] : [];
            const options = context.options || {};
            const selected = this.state.subjectSelection[field.field] ?? null;
            const picker = this.state.pickers[field.field] ?? null;
            const pickerError = this.state.pickerErrors[field.field] ?? "";
            const draft = this.state.drafts[field.field] ?? "";
            const setDraft = value => this.setState(state => ({
                drafts: {...state.drafts, [field.field]: value}
            }));
            const setPicker = value => this.setState(state => ({
                pickers: {...state.pickers, [field.field]: value}
            }));
            const setPickerError = value => this.setState(state => ({
                pickerErrors: {...state.pickerErrors, [field.field]: value}
            }));
            const select = subject => this.setState(state => ({
                subjectSelection: {...state.subjectSelection, [field.field]: subject}
            }));
            const add = subject => {
                if (!subject) {
                    return;
                }
                select(subject);
                if (!chosen.includes(subject)) {
                    this.previewValueChange(field, [...chosen, subject]);
                }
                setPicker(null);
                setDraft("");
                setPickerError("");
            };
            const closePicker = () => {
                setPicker(null);
                setDraft("");
                setPickerError("");
            };
            // What the picker can offer, already in the form the rule stores: a group under its bare
            // name ("anonymous" included), a user as user:<name>.
            const pickerSubjects = picker === "group"
                ? (options.groups || [])
                : (options.users || []).map(name => "user:" + name);
            const addPicked = () => {
                // A select can only hand back one of its options; the check is what keeps a value
                // that is not one of them - from a stale list, say - out of the rule.
                if (!draft || !pickerSubjects.includes(draft)) {
                    setPickerError("Choose a name from the list.");
                    return;
                }
                add(draft);
            };
            const remove = () => {
                if (!selected) {
                    return;
                }
                this.previewValueChange(field, chosen.filter(one => one !== selected));
                select(null);
            };
            return <div className="dialogSubjects">
                {picker
                    ? <div className="modalBackground subjectPickerBackground">
                        <div className="subjectPickerDialog">
                            <div className="title">
                                {picker === "group" ? "Add Group" : "Add User"}
                            </div>
                            <div className="subjectPickerBody">
                                <label className="modalContainerLabel"
                                       htmlFor={field.field + "-picker"}>
                                    {picker === "group" ? "Group:" : "User:"}
                                </label>
                                {/* A real combo box: every value in it exists, so a name that does
                                    not cannot be entered at all, and a typo has nothing to match. */}
                                <select id={field.field + "-picker"}
                                        name={field.field + "-picker"}
                                        value={draft} autoFocus
                                        onChange={(e) => {
                                            setDraft(e.target.value);
                                            setPickerError("");
                                        }}
                                        onKeyDown={(e) => {
                                            if (e.key === "Enter") {
                                                e.preventDefault();
                                                addPicked();
                                            }
                                            if (e.key === "Escape") {
                                                e.preventDefault();
                                                closePicker();
                                            }
                                        }}>
                                    <option value="">
                                        {pickerSubjects.length === 0
                                            ? (picker === "group" ? "No groups exist yet" : "No accounts exist yet")
                                            : (picker === "group" ? "Choose a group" : "Choose a user")}
                                    </option>
                                    {pickerSubjects.map(subject =>
                                        <option key={subject} value={subject}>{subjectLabel(subject)}</option>)}
                                </select>
                            </div>
                            {pickerError
                                ? <div className="subjectPickerError">{pickerError}</div>
                                : null}
                            <div className="modalFooter">
                                <button type="button" className="modalOkButton"
                                        disabled={!draft} onClick={addPicked}>Ok</button>
                                <button type="button" className="modalCancelButton"
                                        onClick={closePicker}>Cancel</button>
                            </div>
                        </div>
                    </div>
                    : null}
                <div className="dialogSubjectMain">
                    <div className="dialogSubjectListBox" role="listbox" aria-label="Applies to"
                         tabIndex={chosen.length ? 0 : -1}
                         onKeyDown={(e) => {
                             if (e.key === "Delete") {
                                 e.preventDefault();
                                 remove();
                             }
                         }}>
                        {chosen.length === 0
                            ? <div className="dialogSubjectEmpty">No users or groups yet.</div>
                            : chosen.map(subject =>
                                <div key={subject} role="option" aria-selected={selected === subject}
                                     className={"dialogSubject" +
                                         (selected === subject ? " dialogSubjectSelected" : "")}
                                     onClick={() => select(subject)}>
                                    <span className="dialogSubjectName">{subjectLabel(subject)}</span>
                                </div>)}
                    </div>
                    <div className="dialogSubjectButtons">
                        <button type="button" className="dataTableAddEditButton"
                                onClick={() => {
                                    setPicker("group");
                                    setDraft("");
                                    setPickerError("");
                                }}>Add Group</button>
                        <button type="button" className="dataTableAddEditButton"
                                onClick={() => {
                                    setPicker("user");
                                    setDraft("");
                                    setPickerError("");
                                }}>Add User</button>
                        <button type="button" className="dataTableRemoveButton"
                                disabled={!selected} onClick={remove}>Remove</button>
                    </div>
                </div>
            </div>;
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

        // Empty, not undefined, when the row has no value yet: a new listener's IP white list is
        // the first such field, and an input that starts its life uncontrolled warns on the first
        // keystroke. An empty text is also exactly what an absent allow list means - anyone.
        return <input type={inputType}
                      name={field.field}
                      value={this.state.values[field.field] ?? ""}
                      onChange={(e) => this.previewValueChange(field, e.target.value)}
                      style={{width: this.inputWidth(field)}}
                      required={context.required !== false}/>
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
                                <label className="modalContainerLabel" htmlFor={field.field}
                                       style={{width: this.props.labelWidth}}>{field.headerName}:</label>
                                : <label/>
                            }
                            {this.makeInputWidget(field)}
                        </div>)
                    }
                </form>
                <div className="modalFooterSpace"/>
                {this.state.error
                    ? <div className="dialogError">{this.state.error}</div>
                    : null}
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
        // A caller that knows what a valid value looks like gets to say so before the dialog
        // closes: a refusal here leaves the typed value in place to be corrected, which a message
        // shown after closing cannot do.
        if (result === "Ok" && this.props.validate) {
            const error = this.props.validate(values);
            if (error) {
                this.setState({error});
                return;
            }
        }
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
        this.setState(state => ({
            values: {
                ...state.values,
                [field.field]: newValue
            },
            // The refusal was about what was just changed.
            error: ""
        }));
    }
}
