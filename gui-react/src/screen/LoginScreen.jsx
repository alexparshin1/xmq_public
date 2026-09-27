import React from 'react';
import {Navigate} from "react-router-dom";
import ControlAPI from "../ControlAPI";
import PasswordInput from "../components/PasswordInput";

/**
 * Sign-in page.
 *
 * The server being signed in to is shown, not entered. The interface is served by the very
 * server it configures, so the page's own origin is the answer, and offering it as a field only
 * created a way to get it wrong: a page served by one server while quietly driving another looks
 * entirely normal, and nothing on screen would say otherwise. To administer a different server,
 * open its interface - the address bar then says which one it is.
 */
export default class LoginScreen extends React.Component {

    state = {
        username: "admin",
        password: "",
        signedIn: false,
        // Nothing is offered until the server has said whether it has been set up: showing a
        // sign-in form for a moment and replacing it is worse than showing nothing for it.
        asked: false
    };

    componentDidMount() {
        // A server that has never been set up has an administrator with no password, and this is
        // the only way in - so the form is not shown at all. It is signed into here instead, and
        // the setup page opens: the alternative is a form whose empty password has to be explained
        // to whoever has just installed the thing.
        ControlAPI.asyncMakeAPICall("SetupState", {}, true)
            .then(result => {
                if (!result || result.setup_required !== true) {
                    this.setState({asked: true});
                    return;
                }
                ControlAPI.login("admin", "")
                    .then(succeeded => this.setState({signedIn: succeeded, asked: true}));
            });
    }

    doLogin() {
        ControlAPI.login(this.state.username, this.state.password)
            .then(succeeded => {
                if (succeeded) {
                    this.setState({signedIn: true});
                }
            });
    }

    render() {
        // Signing in leads to the dashboard rather than back to this page: it is where the server
        // is watched and operated, and it answers the first question anyone has on arriving -
        // whether the server is running. "replace", so Back does not return to a login form that
        // has already been used.
        //
        // Except on a server that has never been set up, where it leads to Initial Setup instead:
        // the administrator account has no password, the interface is answering on this machine
        // only because of it, and nothing else on the dashboard matters until that is fixed.
        if (this.state.signedIn) {
            return <Navigate to={ControlAPI.setupRequired ? "/setup" : "/dashboard"} replace/>;
        }

        // Between asking and being answered. A form that appears and is then taken away reads as
        // a fault; nothing at all reads as a page still loading, which is what it is.
        if (!this.state.asked) {
            return <div/>;
        }

        // Centred as a block in the content area, with the labels right-aligned against their
        // fields and the button centred under the pair of columns rather than under the fields:
        // it acts on the whole form, so it belongs to the form rather than to the last row.
        return <div style={{
            display: "flex",
            justifyContent: "center",
            alignItems: "center",
            minHeight: "60vh"
        }}>
            <table style={{borderSpacing: "8px 6px"}}>
                <tbody>
                <tr>
                    <td style={{textAlign: "right", whiteSpace: "nowrap"}}>Server:</td>
                    <td><b>{ControlAPI.serviceURL}</b></td>
                </tr>
                <tr>
                    <td style={{textAlign: "right"}}>Username:</td>
                    <td><input name="username" style={{width: "200px", boxSizing: "border-box"}}
                               value={this.state.username}
                               onChange={event => this.setState({username: event.target.value})}/></td>
                </tr>
                <tr>
                    <td style={{textAlign: "right"}}>Password:</td>
                    <td><PasswordInput name="password" style={{width: "200px"}}
                                       value={this.state.password}
                                       onChange={event => this.setState({password: event.target.value})}
                                       onKeyDown={event => {
                                           if (event.key === "Enter") {
                                               this.doLogin();
                                           }
                                       }}/></td>
                </tr>
                <tr>
                    <td colSpan={2} style={{textAlign: "center", paddingTop: "10px"}}>
                        <button onClick={() => this.doLogin()}>Login to XMQ</button>
                    </td>
                </tr>
                </tbody>
            </table>
        </div>;
    }
}
