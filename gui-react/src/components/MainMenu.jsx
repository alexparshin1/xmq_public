import React from "react";
import {NavLink} from "react-router-dom";
import ControlAPI from "../ControlAPI";
import AccordionGroup from "./AccordionGroup";

export default class MainMenu extends React.Component {

    state = {
        connected: false
    };

    /**
     * Follows the connection state, so the menu shows what can actually be reached.
     *
     * Subscribed on mount rather than in the constructor, and unsubscribed on unmount: listeners
     * are now kept in a list, so one registered per construction would accumulate.
     */
    componentDidMount() {
        this.unsubscribe = ControlAPI.setOnConnectedChange((isConnected) => {
            this.setState({connected: isConnected});
        });
        this.setState({connected: ControlAPI.connected()});
    }

    componentWillUnmount() {
        if (this.unsubscribe) {
            this.unsubscribe();
        }
    }

    displayMenuItem(invert = false) {
        if (invert) {
            return this.state.connected ? 'none' : 'block';
        }
        return this.state.connected ? 'block' : 'none';
    }

    renderMenuItem(link, text) {
        return <div className="MainMenuItem">
            <NavLink to={link}>{text}</NavLink>
        </div>;
    }

    render() {
        let systemGroup1 = {
            "/login": "Login"
        };
        let systemGroup2 = {
            "/": "Logout"
        };
        // Its own group: the dashboard is where the server is watched and operated, which is a
        // different activity from editing what it will do when it next starts.
        let operationGroup = {
            "/dashboard": "Dashboard",
            "/sessions": "Sessions"
        };
        let configGroup = {
            // First, and named for what it is: it is where a new installation starts, and the one
            // entry here that replaces the configuration rather than editing part of it.
            "/setup": "Initial Setup",
            "/users": "Users",
            "/usergroups": "Groups",
            "/listeners": "Listeners",
            "/sslkeys": "SSL Keys",
            "/persistence": "Persistence",
            "/serverlimits": "Server Limits",
            "/logging": "Logging",
            "/service": "Service",
            "/bridges": "Bridges",
            "/extensions": "Extensions"
        };
        // Documentation is available before signing in: installing and tuning the host are
        // done before there is anything to sign in to.
        let documentationGroup = {
            "/manual": "User Manual"
        };
        return <div className="MainMenu" style={{align: 'left'}}>
            {!this.state.connected
                ? <AccordionGroup name="systemGroup1" title="System" items={systemGroup1}/>
                : null
            }
            {this.state.connected
                ? <AccordionGroup name="systemGroup2" title="System" items={systemGroup2}/>
                : null
            }
            {this.state.connected
                ? <AccordionGroup name="operationGroup" title="Operation" items={operationGroup}/>
                : null
            }
            {this.state.connected
                ? <AccordionGroup name="configGroup" title="Configuration" items={configGroup} collapsed={true}/>
                : null
            }
            <AccordionGroup name="documentationGroup" title="Documentation"
                            items={documentationGroup} collapsed={true}/>
        </div>;
    }
}
