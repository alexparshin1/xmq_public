import React from "react";
import HelpButton from "../components/HelpButton";
import {parameterHelp} from "../help/ParameterHelp";
import "./InputScreen.css";

export default class BasicScreen extends React.Component
{
    /**
     * Renders a numeric input row.
     * When the parameter has a help entry, the row gets a [?] button, the input is bounded by
     * the entry's min and max, and a note appears for values that are allowed but rarely wise.
     * @param {String} label        Field label
     * @param {String} fieldName    Name of the state field to edit
     * @param {String} helpKey      Parameter help key, "<section>.<field>". Optional.
     * @param {String} inputWidth   Input width
     */
    renderInput(label, fieldName, helpKey = null, inputWidth = "70px")
    {
        const help = parameterHelp(helpKey);
        const value = this.state[fieldName];
        const warning = help && help.warning ? help.warning(parseInt(value)) : null;
        const acceptsAuto = (help && help.allowAuto) || value === "auto";

        return <div className="inputScreenRow">
            <label htmlFor={fieldName} className="inputScreenLabel">{label}:</label>
            <input name={fieldName} type={acceptsAuto ? "text" : "number"} style={{width: inputWidth}}
                   min={help ? help.min : undefined}
                   max={help ? help.max : undefined}
                   value={value}
                   onChange={(e) =>
                   {
                       let newState = {};
                       newState[fieldName] = e.target.value;
                       this.setState(newState);
                   }
                   }/>
            <HelpButton helpKey={helpKey}/>
            {warning && <span className="inputScreenWarning">{warning}</span>}
        </div>
    }

    /**
     * Renders a checkbox row, with a [?] button when the parameter has a help entry.
     * @param {String} label        Field label
     * @param {String} fieldName    Name of the state field to edit
     * @param {String} helpKey      Parameter help key, "<section>.<field>". Optional.
     */
    renderCheckBox(label, fieldName, helpKey = null)
    {
        let checked = this.state[fieldName];
        return <div className="inputScreenRow">
            <label htmlFor={fieldName} className="inputScreenLabel">{label}:</label>
            <input id={fieldName} name={fieldName} type="checkbox"
                   checked={checked}
                   onChange={(e) =>
                   {
                       let newState = {};
                       newState[fieldName] = e.target.checked;
                       this.setState(newState);
                   }
                   }/>
            <HelpButton helpKey={helpKey}/>
        </div>
    }
}
