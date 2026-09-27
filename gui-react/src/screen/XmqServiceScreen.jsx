import React from 'react';
import "./InputScreen.css"
import ControlAPI from "../ControlAPI";
import BasicScreen from "./BasicScreen";
import HelpButton from "../components/HelpButton";
import PageTitle from "../components/PageTitle";
import {confirmationWindow, errorWindow} from "../components/MessageWindow";

/**
 * How long the result is left on screen before the browser is sent to the interface's new port.
 * Long enough to read what happened, short enough not to feel stuck.
 */
const redirectDelayMs = 3000;

/**
 * Control service screen.
 * The field matches the WebService type in xmq.wsdl, that is the "web_service" section of
 * the server configuration: the port this interface itself is served on.
 */
export default class XmqServiceScreen extends BasicScreen {
    state = {
        listener_port: 18883,
        // Absent means encrypted: the server treats a configuration that says nothing about it as
        // asking for HTTPS, and this page has to agree with it or the checkbox lies on first load.
        encrypted: true,
        // What came back from the last save, shown on the page rather than in a popup: it names
        // the address the interface has moved to, which is worth having on screen.
        message: "",
        failed: false,
        movedTo: ""
    }

    componentDidMount() {
        ControlAPI.asyncMakeAPICall("ServiceControl", {action: "get"})
            .then(result => {
                if (!result || !result.web_service) {
                    return;
                }
                this.setState({
                    listener_port: result.web_service.listener_port,
                    encrypted: result.web_service.encrypted !== false
                });
            });
    }

    componentWillUnmount() {
        // A page that has been navigated away from has no business sending the browser anywhere.
        if (this.redirectTimer) {
            clearTimeout(this.redirectTimer);
        }
    }

    async saveSettings() {
        const port = parseInt(this.state.listener_port);
        if (!port || port < 1024 || port > 65535) {
            errorWindow("The port number must be between 1024 and 65535.");
            return;
        }

        const encrypted = this.state.encrypted;
        if (!encrypted && !await confirmationWindow(
                "Serve this interface over plain HTTP?\n\n"
                + "It carries the administrator's password and everything the server is "
                + "configured with, none of which is encrypted after this.")) {
            return;
        }

        // Compared against the address this page was actually opened at, rather than against what
        // was loaded into the form: that is what decides whether the browser has to be sent
        // somewhere else, and it is the only thing that does.
        const scheme = encrypted ? "https:" : "http:";
        const addressChanges = port !== parseInt(ControlAPI.defaultPort()) ||
                               scheme !== window.location.protocol;

        this.setState({message: "", failed: false, movedTo: ""});

        ControlAPI.asyncMakeAPICall("ServiceControl", {
            action: "set",
            web_service: {listener_port: port, encrypted: encrypted}
        }).then(result => {
            if (result === null) {
                // XMQ server is offline
                return;
            }
            const error = ControlAPI.apiError(result);
            if (error) {
                this.setState({failed: true, message: "Can't change the service settings: " + error});
                return;
            }

            if (result.service_restart_required) {
                // Saved, but the running interface could not take it - the port is held by
                // something else, or a certificate could not be prepared. It stays where it is.
                this.setState({
                    failed: true,
                    message: (result.message || "The interface could not be changed.") +
                             "\nIt is still served at " + window.location.protocol + "//"
                             + window.location.hostname + ":" + ControlAPI.defaultPort() +
                             ", and stays there until the XMQ service is restarted."
                });
                return;
            }

            if (!addressChanges) {
                this.setState({message: "The service settings are unchanged."});
                return;
            }

            // The interface has moved. The browser follows it, rather than leaving someone on a
            // page whose every call now goes somewhere nothing is listening.
            const movedTo = ControlAPI.interfaceUrl(port, scheme);
            this.redirectTimer = setTimeout(() => window.location.assign(movedTo), redirectDelayMs);
            this.setState({
                movedTo: movedTo,
                message: "The interface is now served at " + movedTo + ". Taking you there now."
                         + (encrypted ? "\nThe browser may ask about the certificate again."
                                      : "")
            });
        });
    }

    render() {
        return <div className="inputScreen">
            <PageTitle>Service</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    The port this configuration interface is served on. It is not an MQTT port:
                    clients connect on the ports set up under Listeners.
                </p>
                <p>
                    Served over HTTPS unless that is turned off here. Unless a certificate has been
                    installed under SSL Keys, it is one the server issued to itself, which browsers
                    warn about on the first visit &mdash; the fingerprint to check it against is on
                    that page and in the server log.
                </p>
                <p>
                    Both changes take effect at once, and this page follows the interface to
                    wherever it has gone; connections already open are served to the end. If
                    something else already holds the port, or a certificate cannot be prepared, the
                    interface stays where it is and says so.
                </p>
            </div>

            {this.renderInput("Service port number", "listener_port",
                              "web_service.listener_port", "90px")}

            <div className="inputScreenRow">
                <label htmlFor="encrypted" className="inputScreenLabel">Serve over HTTPS:</label>
                <input name="encrypted" type="checkbox"
                       checked={this.state.encrypted}
                       onChange={e => this.setState({encrypted: e.target.checked})}/>
                <HelpButton helpKey="web_service.encrypted"/>
            </div>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" onClick={e => this.saveSettings()}>
                    Save
                </button>
            </div>

            {this.state.message
                ? <div style={{
                    textAlign: "left", maxWidth: "640px", margin: "8px 10px", whiteSpace: "pre-wrap",
                    color: this.state.failed ? "#c62828" : "#2e7d32"
                }}>
                    {this.state.message}
                    {/* Offered as a link as well as followed automatically: a browser that blocks
                        the redirect, or a host reached under a different name, still leaves
                        somewhere to click. */}
                    {this.state.movedTo
                        ? <div style={{marginTop: "8px"}}>
                            <a href={this.state.movedTo}>{this.state.movedTo}</a>
                        </div>
                        : null}
                </div>
                : null}
        </div>
    }
}
