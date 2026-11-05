import React from "react";
import {parameterHelp} from "../help/ParameterHelp";
import MessageSign from "./MessageSign";
import "../components/DialogWindow.css";
import "./HelpButton.css";

/**
 * A [?] button that opens a popup explaining a configuration parameter.
 *
 * The wording, range, and suggested value normally come from the parameter help catalog, so the
 * screens only need to name the parameter with helpKey.
 *
 * An extension's settings are the exception: what they mean is declared by the extension itself and
 * cannot be in a catalogue this project ships. Those pass `title` and `text` directly. One
 * component either way, so the button looks and behaves the same wherever it appears - a second [?]
 * of its own design would read as a different thing.
 */
/**
 * One sentence per line.
 *
 * A setting's help arrives as a single string and a parameter's as a paragraph: several sentences
 * run together in a 460-pixel box read as one long line, and the one that matters is the one the
 * eye skips. Line breaks are kept too, since the text may have been written with them.
 *
 * @param {string} text
 * @return {string[]} the sentences, without empty ones.
 */
function sentences(text)
{
    return String(text)
        .split(/\n+|(?<=[.!?])\s+/)
        .map(line => line.trim())
        .filter(line => line !== "");
}

export default class HelpButton extends React.Component
{
    state = {
        showHelp: false
    };

    renderHelpFact(label, value)
    {
        if (!value)
        {
            return null;
        }
        return <div className="helpFact">
            <span className="helpFactLabel">{label}:</span>
            <span>{value}</span>
        </div>;
    }

    renderPopup(help)
    {
        return <div className="modalBackground">
            <div className="modalContainer helpContainer">
                <div className="title">{help.title}</div>
                <div className="helpContent">
                    <div className="helpSign">
                        <MessageSign kind="information" size={40}/>
                    </div>
                    <div className="helpBody">
                        {help.text.flatMap((paragraph, index) =>
                            sentences(paragraph).map((sentence, at) =>
                                <p key={index + "." + at}>{sentence}</p>))}
                        {this.renderHelpFact("Range", help.range)}
                        {this.renderHelpFact("Suggested", help.suggested)}
                    </div>
                </div>
                <div className="modalFooter">
                    <button type="button" className="modalOkButton"
                            onClick={() => this.setState({showHelp: false})}>
                        Ok
                    </button>
                </div>
            </div>
        </div>;
    }

    render()
    {
        const help = this.props.helpKey
            ? parameterHelp(this.props.helpKey)
            : (this.props.text
                ? {title: this.props.title, text: [this.props.text]}
                : null);
        if (!help)
        {
            return null;
        }

        return <span>
            <button type="button" className="helpButton"
                    title={"About " + help.title}
                    onClick={() => this.setState({showHelp: true})}>
                ?
            </button>
            {this.state.showHelp && this.renderPopup(help)}
        </span>;
    }
}
