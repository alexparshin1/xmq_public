import React from 'react';
import {confirmationWindow, errorWindow} from "../components/MessageWindow";
import ControlAPI from "../ControlAPI";
import HelpButton from "../components/HelpButton";
import "./InputScreen.css"
import BasicScreen from "./BasicScreen";
import PageTitle from "../components/PageTitle";

/**
 * Initial setup screen.
 *
 * Every other Configuration page edits one section of a configuration that already exists. This
 * one builds a configuration from scratch: it takes the few settings a working server cannot be
 * without, fills everything else in from xmq_server.conf.template, and restarts the server on it.
 *
 * The fields match the InitialSetup type in xmq.wsdl.
 */
export default class InitialSetupScreen extends BasicScreen {

    state = {
        // The administrator account, first because it is what the interface is reached through:
        // there is no point in ports and a node name nobody can sign in to change afterwards.
        admin_password: "",
        admin_password_confirmation: "",
        node_name: "",
        // The address other machines reach this server at, which is not the same as the name it
        // is labelled with. Filled in from the server, which offers this machine's host name.
        node_host: "",
        mqtt_port: 1883,
        mqtt_ssl_port: 8883,
        web_service_port: 18883,
        redis_host: "",
        redis_port: 6379,
        // Set while the request is in flight: applying replaces the configuration and restarts
        // the server, which is not something to have two of running at once.
        busy: false,
        // What came back from the last submission, shown on the page rather than in a popup:
        // it usually needs reading twice and acting on.
        message: "",
        failed: false,
        // Whether this is a server that has never been set up, as the sign-in reported. It
        // changes what this page is: on a fresh installation it is the first thing to do and
        // replaces nothing, rather than the page that throws a working configuration away.
        // Read once, at construction, so the page does not change character under the result
        // of its own submission.
        firstRun: ControlAPI.setupRequired
    };

    /**
     * Loads the values a new configuration would start from.
     *
     * These come from the installed template, not from the configuration in use: the page offers
     * a fresh start, and starting it from the settings being replaced would defeat the point.
     */
    componentDidMount() {
        ControlAPI.asyncMakeAPICall("InitialSetupControl", {action: "get"})
            .then(result => {
                if (!result) {
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    this.setState({failed: true, message: "Can't read the configuration template: " + error});
                    return;
                }
                if (result.setup) {
                    this.setState({...result.setup});
                }
            });
    }

    /**
     * Describes what is wrong with the form, or returns null when it can be submitted.
     * @return {?string} the reason, or null.
     */
    passwordRef = React.createRef();

    /**
     * Whether the server refused what was typed as a password rather than something else.
     * @param {string} failure  What the server said.
     * @return {boolean} true when the password is what it objected to.
     */
    refusedThePassword(failure) {
        return /password/i.test(failure);
    }

    validationError() {
        if (!this.state.admin_password) {
            return "The administrator password is required.";
        }
        if (this.state.admin_password !== this.state.admin_password_confirmation) {
            return "The two administrator passwords do not match.";
        }

        // The same pattern the Name type in xmq.wsdl carries, so the server does not have to be
        // asked to find out that a name will be refused.
        if (!/^\w[\w.-]+$/.test(this.state.node_name)) {
            return "The node name must start with a letter, digit, or underscore, be at least two " +
                   "characters long, and contain only letters, digits, dots, dashes, and underscores.";
        }

        // Checked here as well as on the server: the ports are asked for separately, and an
        // address carrying one of its own would produce an address with two.
        if (/[:\s/]/.test(this.state.node_host)) {
            return "The node address must be a host name or address without a port: the ports " +
                   "are set below.";
        }

        const ports = {
            "MQTT port": parseInt(this.state.mqtt_port),
            "MQTT+SSL port": parseInt(this.state.mqtt_ssl_port),
            "Web interface port": parseInt(this.state.web_service_port)
        };
        for (const [name, port] of Object.entries(ports)) {
            if (!port || port < 1024 || port > 65535) {
                return "The " + name + " must be between 1024 and 65535.";
            }
        }
        const portNumbers = Object.values(ports);
        if (new Set(portNumbers).size !== portNumbers.length) {
            return "The MQTT, MQTT+SSL, and web interface ports must all differ.";
        }

        if (this.state.redis_host) {
            const redisPort = parseInt(this.state.redis_port);
            if (!redisPort || redisPort < 1 || redisPort > 65535) {
                return "The Redis port must be between 1 and 65535.";
            }
        }

        return null;
    }

    /**
     * Replaces the configuration and restarts the server.
     *
     * Confirmed first, and named in the confirmation: this is the one page that throws settings
     * away rather than changing them, and it does so the moment the button is pressed.
     */
    async submit() {
        const error = this.validationError();
        if (error) {
            errorWindow(error);
            return;
        }

        const servicePort = parseInt(this.state.web_service_port);

        // Not asked on a server that has never been set up: there is nothing there to replace,
        // and a warning about losing a configuration nobody has made yet only reads as a reason
        // to stop. Everywhere else this page is the one destructive page in the interface.
        if (!this.state.firstRun && !await confirmationWindow(
            "This replaces the entire server configuration with a new one.\n\n" +
            "Listeners, bridges, limits, and logging settings all go back to their installed " +
            "defaults. Every user account is removed except admin, which is set to the password " +
            "entered above, so MQTT clients that authenticate will need their accounts adding " +
            "again. Installed certificates are kept.\n\n" +
            "The server is then restarted, disconnecting every client.\n\n" +
            "Continue?")) {
            return;
        }

        this.setState({busy: true, message: "", failed: false});

        ControlAPI.asyncMakeAPICall("InitialSetupControl", {
            action: "set",
            setup: {
                admin_password: this.state.admin_password,
                node_name: this.state.node_name,
                node_host: this.state.node_host,
                mqtt_port: parseInt(this.state.mqtt_port),
                mqtt_ssl_port: parseInt(this.state.mqtt_ssl_port),
                web_service_port: servicePort,
                redis_host: this.state.redis_host,
                redis_port: this.state.redis_host ? parseInt(this.state.redis_port) : 0
            }
        }).then(result => {
            if (result === null) {
                // The interface itself is unreachable, which is a different problem.
                this.setState({busy: false});
                return;
            }

            const failure = ControlAPI.apiError(result);
            if (failure) {
                // A password the server will not accept is answered on the spot rather than left
                // in the page's message area: it is the one refusal the person can fix without
                // reading anything else, and the field it belongs to is put back under the cursor.
                // The rules are the server's - keeping a second copy of them here is how the two
                // come to disagree about what a good password is.
                if (this.refusedThePassword(failure)) {
                    errorWindow(failure);
                    this.setState({busy: false});
                    if (this.passwordRef.current) {
                        this.passwordRef.current.focus();
                        this.passwordRef.current.select();
                    }
                    return;
                }

                this.setState({
                    busy: false, failed: true,
                    message: "The configuration was not replaced: " + failure
                });
                return;
            }

            // Nothing is shown here and nothing more is asked of the server. The password has
            // just changed, so this session is over, and the interface is now presenting a
            // certificate this browser has never seen: every further call would fail, and each
            // failure raises "XMQ server is offline" over whatever is on screen. What needed
            // saying was said above the form, before any of this was set in motion.
            //
            // Where to: the port the interface is on now. When it could not take the new one it
            // is still on the old, and that is where the browser has to go.
            window.location.assign(result.service_restart_required
                                   ? ControlAPI.freshInterfaceUrl(parseInt(ControlAPI.defaultPort()))
                                   : ControlAPI.freshInterfaceUrl(servicePort));
        });
    }

    render() {
        return <div className="inputScreen">
            <PageTitle>Initial Setup</PageTitle>

            {this.state.firstRun
                ? <div style={{
                    textAlign: "left", maxWidth: "640px", margin: "0 10px 12px 10px", padding: "10px",
                    border: "1px solid #1565c0", borderRadius: "6px", color: "#1565c0"
                }}>
                    <b>This server has not been set up yet.</b>
                    <p style={{marginBottom: 0}}>
                        The <code>admin</code> account has no password, which is why this interface
                        let you in without one &mdash; and why it is answering on this machine only.
                        Nothing else can reach it, and the account cannot be used over MQTT at all.
                    </p>
                    <p style={{marginBottom: 0}}>
                        Set a password below. The interface then starts accepting connections from
                        the network, and the settings on this page become the server&apos;s
                        configuration. Everything here can be changed afterwards from the other
                        Configuration pages.
                    </p>
                </div>
                : <div style={{
                    textAlign: "left", maxWidth: "640px", margin: "0 10px 12px 10px", padding: "10px",
                    border: "1px solid #c62828", borderRadius: "6px", color: "#c62828"
                }}>
                    <b>The current configuration will be lost.</b>
                    <p style={{marginBottom: 0}}>
                        This page does not edit the configuration &mdash; it builds a new one from the
                        installed template. Listeners, bridges, limits, logging settings, and SSL key
                        paths all go back to their defaults, and the server is restarted on the result,
                        disconnecting every client.
                    </p>
                    <p style={{marginBottom: 0}}>
                        The user accounts go too. What is left is <code>admin</code>, with the password
                        entered below, and the internal <code>cluster</code> account &mdash; so every
                        MQTT client that authenticates will need its account adding again. Installed
                        certificate files are kept: they are files of their own, not configuration.
                    </p>
                </div>}

            <div className="inputScreenIntro">
                <p><b>What happens when you submit, in order:</b> the settings are written and the
                    server is restarted; the interface is issued a new certificate, made out to
                    this server&apos;s address, so your browser will ask you to accept it again;
                    and the session ends, because it carries the password it signed in with. You
                    are taken to the interface, where you sign in with the new password.
                </p>
                <p>
                    The values below are the ones the template defines. Change what needs changing
                    for this host, and submit.
                </p>
                <p>
                    This page needs the interface to be running, which is no help when the
                    configuration is what stops the server from starting. The same reset is
                    available from the command line, where nothing needs to be up:{" "}
                    <code>xmq_server --reset-configuration</code>.
                </p>
            </div>

            <fieldset>
                <legend>Administrator</legend>
                <div className="inputScreenIntro">
                    <p>
                        The <code>admin</code> account is what this interface is administered
                        through. Setup creates it if it is not there, and gives it the password
                        below either way, so a configuration always comes with a way in.
                    </p>
                </div>
                <div className="inputScreenRow">
                    <label className="inputScreenLabel">Username:</label>
                    <span><b>admin</b></span>
                    <HelpButton helpKey="initial_setup.admin_password"/>
                </div>
                <div className="inputScreenRow">
                    <label htmlFor="admin_password" className="inputScreenLabel">Password:</label>
                    <input name="admin_password" type="password" autoComplete="new-password"
                           ref={this.passwordRef}
                           value={this.state.admin_password}
                           onChange={(e) => this.setState({admin_password: e.target.value})}/>
                </div>
                <div className="inputScreenRow">
                    <label htmlFor="admin_password_confirmation" className="inputScreenLabel">
                        Repeat password:
                    </label>
                    <input name="admin_password_confirmation" type="password" autoComplete="new-password"
                           value={this.state.admin_password_confirmation}
                           onChange={(e) => this.setState({admin_password_confirmation: e.target.value})}/>
                </div>
            </fieldset>

            <fieldset>
                <legend>This Server</legend>
                <div className="inputScreenRow">
                    <label htmlFor="node_name" className="inputScreenLabel">Node name:</label>
                    <input name="node_name" type="text" style={{width: '200px'}}
                           value={this.state.node_name}
                           onChange={(e) => this.setState({node_name: e.target.value})}/>
                    <HelpButton helpKey="initial_setup.node_name"/>
                </div>

                <div className="inputScreenRow">
                    <label htmlFor="node_host" className="inputScreenLabel">Node address:</label>
                    <input name="node_host" type="text" style={{width: '200px'}}
                           value={this.state.node_host}
                           onChange={(e) => this.setState({node_host: e.target.value})}/>
                    <HelpButton helpKey="initial_setup.node_host"/>
                </div>
            </fieldset>

            <fieldset>
                <legend>Ports</legend>
                {this.renderInput("MQTT port", "mqtt_port", "initial_setup.mqtt_port", "90px")}
                {this.renderInput("MQTT+SSL port", "mqtt_ssl_port", "initial_setup.mqtt_ssl_port", "90px")}
                {this.renderInput("Web interface port", "web_service_port",
                                  "web_service.listener_port", "90px")}
            </fieldset>

            <fieldset>
                <legend>Redis</legend>
                <div className="inputScreenIntro">
                    <p>
                        Optional. Leave the host empty to set the server up without persistence:
                        sessions and queued messages are then kept in memory only.
                    </p>
                </div>
                <div className="inputScreenRow">
                    <label htmlFor="redis_host" className="inputScreenLabel">Redis host:</label>
                    <input name="redis_host" type="text"
                           value={this.state.redis_host}
                           onChange={(e) => this.setState({redis_host: e.target.value})}/>

                    <label htmlFor="redis_port" className="inputScreenLabel"
                           style={{width: '50px'}}>port:</label>
                    <input name="redis_port" type="number" style={{width: '80px'}}
                           value={this.state.redis_port}
                           onChange={(e) => this.setState({redis_port: e.target.value})}/>
                    <HelpButton helpKey="persistence.redis_host"/>
                </div>
            </fieldset>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" style={{width: "180px"}}
                        disabled={this.state.busy}
                        onClick={() => this.submit()}>
                    {this.state.busy ? "Working..." : "Submit and restart"}
                </button>
            </div>

            {this.state.message
                ? <div style={{
                    textAlign: "left", maxWidth: "640px", margin: "8px 10px", whiteSpace: "pre-wrap",
                    color: this.state.failed ? "#c62828" : "#2e7d32"
                }}>
                    {this.state.message}
                </div>
                : null}
        </div>;
    }
}
