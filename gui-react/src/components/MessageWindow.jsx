import React from "react";
import {createRoot} from "react-dom/client";
import MessageSign from "./MessageSign";
import "./DialogWindow.css";
import "./MessageWindow.css";

/**
 * A message with a sign beside it and a button under it.
 *
 * The three kinds differ only in the sign and the buttons: what makes a message an error rather
 * than a remark is the sign, and having one component means they cannot drift apart in anything
 * else.
 */
export class MessageWindow extends React.Component {

    componentDidMount() {
        document.addEventListener("keydown", this.onKeyDown);
    }

    componentWillUnmount() {
        document.removeEventListener("keydown", this.onKeyDown);
    }

    /**
     * Escape answers with the last button, which is the one that changes nothing.
     *
     * confirm() closed on Escape, and a window that can only be dismissed with the mouse is a
     * worse answer than the one it replaces.
     */
    onKeyDown = event => {
        if (event.key === "Escape") {
            const buttons = this.props.buttons;
            this.props.onClose(buttons[buttons.length - 1].answer);
        }
    };

    render() {
        const buttons = this.props.buttons;

        return <div className="modalBackground">
            <div className="modalContainer messageWindow">
                <div className="title">{this.props.title}</div>

                <div className="messageWindowBody">
                    <div className="messageWindowSign">
                        <MessageSign kind={this.props.sign}/>
                    </div>
                    {/* The message keeps its line breaks: a server can answer with more than one
                        sentence, and running them together loses the shape it was given. */}
                    <div className="messageWindowText">{this.props.message}</div>
                </div>

                <div className="modalFooter messageWindowFooter">
                    {buttons.map((button, at) =>
                        <button key={button.label}
                                className={at === 0 ? "modalOkButton" : "modalCancelButton"}
                                autoFocus={at === 0}
                                onClick={() => this.props.onClose(button.answer)}>{button.label}</button>)}
                </div>
            </div>
        </div>;
    }
}

/**
 * Confirms a start or a stop of the service, and says whether the stored state should be discarded.
 *
 * Two answers in one window rather than a warning followed by a question: the warning used to say
 * that a start would discard this node's state because persistence.clean_start was set, and left
 * the operator with nowhere to say "not this time" - the setting had to be found and changed on
 * another page first. The tick is the answer for this action only; the setting is left as it is.
 *
 * A stop asks for the same reason a start does: stopping normally keeps the sessions and their
 * queued messages, and a broker being taken out for good is the case where they should go.
 */
class ServiceWindow extends React.Component {
    state = {clean: this.props.cleanByDefault === true};

    componentDidMount() {
        document.addEventListener("keydown", this.onKeyDown);
    }

    componentWillUnmount() {
        document.removeEventListener("keydown", this.onKeyDown);
    }

    onKeyDown = event => {
        if (event.key === "Escape") {
            this.props.onClose({confirmed: false, clean: this.state.clean});
        }
    };

    render() {
        return <div className="modalBackground">
            <div className="modalContainer messageWindow">
                <div className="title">{this.props.title}</div>

                <div className="messageWindowBody">
                    <div className="messageWindowSign">
                        <MessageSign kind="question"/>
                    </div>
                    <div className="messageWindowText">
                        {this.props.message}
                        <label className="messageWindowChoice">
                            <input type="checkbox"
                                   checked={this.state.clean}
                                   onChange={event => this.setState({clean: event.target.checked})}/>
                            {" "}Clean broker/cluster state
                        </label>
                        <div className="messageWindowChoiceHint">{this.props.hint}</div>
                    </div>
                </div>

                <div className="modalFooter messageWindowFooter">
                    <button className="modalOkButton"
                            autoFocus
                            onClick={() => this.props.onClose({confirmed: true, clean: this.state.clean})}>Ok</button>
                    <button className="modalCancelButton"
                            onClick={() => this.props.onClose({confirmed: false, clean: this.state.clean})}>Cancel</button>
                </div>
            </div>
        </div>;
    }
}

/**
 * Shows one and waits for the answer.
 *
 * Called the way alert() and confirm() were, which is why it hands back a promise rather than
 * asking every page to hold a piece of state for a message it shows once. The window is mounted
 * into a container of its own and taken down again with the answer.
 *
 * @param {Object} properties  sign, title, message and buttons.
 * @return {Promise<boolean>} what the button pressed says.
 */
function show(properties) {
    return new Promise(resolve => {
        const container = document.createElement("div");
        document.body.appendChild(container);
        const root = createRoot(container);

        const close = answer => {
            // Unmounted after this call returns rather than inside it: React refuses to take a
            // root down while it is rendering the very tree that asked.
            setTimeout(() => {
                root.unmount();
                container.remove();
            }, 0);
            resolve(answer);
        };

        const Window = properties.component || MessageWindow;
        root.render(<Window {...properties} onClose={close}/>);
    });
}

/**
 * Asks whether to start or stop the service, and whether to discard the stored state.
 *
 * @param {Object} options
 * @param {boolean} options.stop            Ask about a stop rather than a start.
 * @param {boolean} options.cleanByDefault  What the tick starts as, for a start.
 * @param {boolean} options.stored          Whether this node has stored state at all; when it has
 *                                          none the tick is not offered, because it would do nothing.
 * @return {Promise<{confirmed: boolean, clean: boolean}>}
 */
export function serviceWindow(options = {}) {
    const stop = options.stop === true;
    return show({
        component: ServiceWindow,
        sign: "question",
        title: stop ? "Stop service" : "Start service",
        message: stop
            ? "Please confirm that you want to stop service."
            : "Please confirm that you want to (re)start service.",
        hint: stop
            ? "Discards this node's stored sessions and queued messages instead of keeping them for " +
              "when it starts again. Other nodes' data and the rest of the Redis database are preserved."
            : "Discards this node's stored sessions and queued messages. Other nodes' data and the " +
              "rest of the Redis database are preserved.",
        cleanByDefault: stop ? false : options.cleanByDefault === true
    });
}

/** Something failed. */
export function errorWindow(message, title = "Error") {
    return show({sign: "error", title, message, buttons: [{label: "Close", answer: false}]});
}

/** Something is worth stopping for, though nothing has failed yet. */
export function warningWindow(message, title = "Warning") {
    return show({sign: "warning", title, message, buttons: [{label: "Close", answer: false}]});
}

/** Something worth saying that did not fail. */
export function informationWindow(message, title = "Information") {
    return show({sign: "information", title, message, buttons: [{label: "Close", answer: false}]});
}

/**
 * A question with two answers.
 * @return {Promise<boolean>} true when Ok was pressed.
 */
export function confirmationWindow(message, title = "Confirm") {
    return show({
        sign: "question", title, message,
        buttons: [{label: "Ok", answer: true}, {label: "Cancel", answer: false}]
    });
}
