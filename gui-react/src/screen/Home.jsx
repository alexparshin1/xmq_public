import React from 'react';
import ControlAPI from "../ControlAPI";
import {NavLink} from "react-router-dom";

export default class HomeScreen extends React.Component {

    /**
     * This page is also the Logout menu item, so reaching it ends the session.
     *
     * Done on mount rather than in the constructor: signing out notifies listeners, and doing that
     * while another component is rendering is what React warns about.
     */
    componentDidMount() {
        ControlAPI.logout();
    }

    render() {
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
