import React from "react";
import './PasswordInput.css';

/**
 * A password field with a button that shows what has been typed.
 *
 * The button exists because a password typed blind is a password typed twice: the field that
 * refuses to show its contents is also the field where a wrong keyboard layout, a held shift, or
 * a trailing space is invisible until the server refuses the login. The whole point is to make
 * the mistake visible before the round trip, so the field starts masked and only shows the value
 * while somebody is holding the question open.
 *
 * Every prop other than the ones named below goes straight to the input, so this is a drop-in
 * replacement for <input type="password"> - name, value, onChange, style, placeholder, required,
 * onKeyDown and the rest keep working as they did.
 */
export default class PasswordInput extends React.Component {

    state = {visible: false};

    render() {
        // The wrapper is what positions the button, so it takes the class name a caller means for
        // the field as a whole; the input keeps its own width and the rest of its props.
        const {className, wrapperClassName, ...inputProps} = this.props;
        const visible = this.state.visible;
        const label = visible ? "Hide password" : "Show password";

        return <span className={"passwordInput" + (wrapperClassName ? " " + wrapperClassName : "")}>
            <input {...inputProps}
                   type={visible ? "text" : "password"}
                   className={"passwordInputField" + (className ? " " + className : "")}/>
            {/* type="button": several of these live inside a <form>, and a button without it is a
                submit button, so revealing the password would send the form. tabIndex -1 keeps it
                out of the tab order - tab from a password field belongs on the next field, not on
                a button that shows the password to whoever is behind you. */}
            <button type="button" className="passwordInputEye" tabIndex={-1}
                    title={label} aria-label={label} aria-pressed={visible}
                    onClick={() => this.setState({visible: !visible})}>
                {visible ? PasswordInput.eyeOffIcon() : PasswordInput.eyeIcon()}
            </button>
        </span>;
    }

    // Drawn here rather than loaded: an icon font or an image file for two glyphs is a request the
    // browser has to make before the field looks finished.
    static eyeIcon() {
        return <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor"
                    strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
            <path d="M1.5 12S5 5.5 12 5.5 22.5 12 22.5 12 19 18.5 12 18.5 1.5 12 1.5 12Z"/>
            <circle cx="12" cy="12" r="3.2"/>
        </svg>;
    }

    static eyeOffIcon() {
        return <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor"
                    strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
            <path d="M9.9 5.7A9.9 9.9 0 0 1 12 5.5c7 0 10.5 6.5 10.5 6.5a18.6 18.6 0 0 1-3.9 4.8"/>
            <path d="M6.4 7.3A18.4 18.4 0 0 0 1.5 12S5 18.5 12 18.5a10 10 0 0 0 4.2-.9"/>
            <path d="M9.8 9.9a3.2 3.2 0 0 0 4.4 4.4"/>
            <path d="M3 3l18 18"/>
        </svg>;
    }
}
