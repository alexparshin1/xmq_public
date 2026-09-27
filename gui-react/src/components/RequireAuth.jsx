import React from "react";
import {Navigate} from "react-router-dom";
import ControlAPI from "../ControlAPI";

/**
 * Shows its content only while signed in, and sends the browser to the login page otherwise.
 *
 * Without it the menu is the only thing that reflects being signed out: the routed page stays on
 * screen after the session ends, still showing whatever it last read from the server - a
 * configuration page that can no longer save, or a list of sessions that may since have changed.
 * It also covers a page opened straight from a bookmark or a typed URL, which never passed
 * through the menu at all.
 *
 * Re-renders on connection changes, so an expiring session moves the page by itself rather than
 * waiting for the next click.
 */
export default class RequireAuth extends React.Component {
    state = {
        connected: ControlAPI.connected()
    };

    componentDidMount() {
        this.unsubscribe = ControlAPI.setOnConnectedChange((isConnected) => {
            this.setState({connected: isConnected});
        });
        // The state was read before the listener existed; re-read in case it changed in between.
        this.setState({connected: ControlAPI.connected()});
    }

    componentWillUnmount() {
        if (this.unsubscribe) {
            this.unsubscribe();
        }
    }

    render() {
        if (!this.state.connected) {
            // replace, so that going Back does not return to a page that cannot work.
            return <Navigate to="/login" replace/>;
        }
        return this.props.children;
    }
}
