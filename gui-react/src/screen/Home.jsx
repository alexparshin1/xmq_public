import React from 'react';
import ControlAPI from "../ControlAPI";
import {NavLink, Navigate} from "react-router-dom";

/**
 * The landing page, and the Logout menu item.
 *
 * Reaching it ends the session. Done on mount rather than in the constructor: signing out
 * notifies listeners, and doing that while another component is rendering is what React warns
 * about.
 *
 * A server that has never been set up is sent to the sign-in page instead of being shown this
 * one: the sign-in page signs the administrator in without a password and opens the Initial
 * Setup page, and that is the one thing worth doing on a fresh installation. Leaving the visitor
 * here made them find the Login menu item first, which reads as an interface that does nothing
 * until it is poked.
 */
export default class HomeScreen extends React.Component {

    state = {
        // null until the server has answered: the SetupState call is what tells a fresh
        // installation from a configured one.
        setupRequired: null
    };

    componentDidMount() {
        ControlAPI.logout();

        // Quietly: nobody asked for this read, and a failure only means this page stays as it is.
        ControlAPI.asyncMakeAPICall("SetupState", {}, true)
            .then(result => {
                this.setState({setupRequired: result !== null && result.setup_required === true});
            });
    }

    render() {
        if (this.state.setupRequired === true) {
            return <Navigate to="/login" replace/>;
        }

        return (
            <div style={{textAlign: 'Left'}}>
                <p>
                    Welcome to XMQ server control interface.
                </p>
                <p>
                    The control interface allows you to control the XMQ server without having to restart it. Any changes
                    in server configuration are passed to XMQ server and also stored in its
                    configuration.
                </p>
                <p>
                    Please <NavLink to="/login">login</NavLink> as a user with XMQ server administrative privileges.
                </p>
            </div>
        );
    }
}
