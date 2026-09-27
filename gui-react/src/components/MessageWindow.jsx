import React from "react";
import {createRoot} from "react-dom/client";
import "./DialogWindow.css";
import "./MessageWindow.css";

/**
 * The signs, drawn rather than loaded.
 *
 * Inline SVG so that a message can be shown before anything else has finished loading - which is
 * exactly when things go wrong - and so that the drawing scales with the box rather than blurring.
 */
const signs = {
    error: <svg viewBox="0 0 64 64" width="64" height="64" role="img" aria-label="Error">
        <polygon points="20,2 44,2 62,20 62,44 44,62 20,62 2,44 2,20"
                 fill="#c62828" stroke="#8e0000" strokeWidth="2"/>
        <rect x="14" y="28" width="36" height="8" rx="2" fill="white"/>
    </svg>,

    question: <svg viewBox="0 0 64 64" width="64" height="64" role="img" aria-label="Question">
        <circle cx="32" cy="32" r="30" fill="#1565c0" stroke="#0d47a1" strokeWidth="2"/>
        <text x="32" y="46" textAnchor="middle" fontSize="42" fontWeight="bold"
              fontFamily="serif" fill="white">?</text>
    </svg>,

    information: <svg viewBox="0 0 64 64" width="64" height="64" role="img" aria-label="Information">
        <circle cx="32" cy="32" r="30" fill="#2e7d32" stroke="#1b5e20" strokeWidth="2"/>
        <text x="32" y="47" textAnchor="middle" fontSize="42" fontWeight="bold"
              fontFamily="serif" fill="white">i</text>
    </svg>
};

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
                    <div className="messageWindowSign">{signs[this.props.sign]}</div>
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

        root.render(<MessageWindow {...properties} onClose={close}/>);
    });
}

/** Something failed. */
export function errorWindow(message, title = "Error") {
    return show({sign: "error", title, message, buttons: [{label: "Close", answer: false}]});
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
