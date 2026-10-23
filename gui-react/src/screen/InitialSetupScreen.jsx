import React from 'react';
import {confirmationWindow, errorWindow} from "../components/MessageWindow";
import ControlAPI from "../ControlAPI";
import HelpButton from "../components/HelpButton";
import "./InputScreen.css"
import BasicScreen from "./BasicScreen";
import PageTitle from "../components/PageTitle";
import PasswordInput from "../components/PasswordInput";

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
        cluster_password: "",
        cluster_password_confirmation: "",
        admin_confirmation_checked: false,
        cluster_confirmation_checked: false,
        node_name: "",
        // The address other machines reach this server at, which is not the same as the name it
        // is labelled with. Filled in from the server, which offers this machine's host name.
        node_host: "",
        mqtt_port: 1883,
        mqtt_ssl_port: 8883,
        web_service_port: 18883,
        redis_host: "",
        redis_port: 6379,
        redisCheck: null,
        database_type: "",
        database_host: "",
        database_name: "xmq",
        database_username: "xmq",
        database_password: "",
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
                    const {database_uri, ...setup} = result.setup;
                    if (database_uri && !database_uri.startsWith("sqlite3://")) {
                        const uri = new URL(database_uri);
                        Object.assign(setup, {
                            database_type: uri.protocol.slice(0, -1),
                            database_host: uri.host,
                            database_name: decodeURIComponent(uri.pathname.slice(1)) || "xmq",
                            database_username: decodeURIComponent(uri.username),
                            database_password: decodeURIComponent(uri.password)
                        });
                    }
                    this.setState(setup);
                }
            });
    }

    databaseUri() {
        if (!this.state.database_type) {
            return "";
        }
        const username = encodeURIComponent(this.state.database_username);
        const password = encodeURIComponent(this.state.database_password);
        const credentials = username || password ? `${username}:${password}@` : "";
        return `${this.state.database_type}://${credentials}${this.state.database_host.trim()}/` +
               encodeURIComponent(this.state.database_name.trim());
    }

    /**
     * Describes what is wrong with the form, or returns null when it can be submitted.
     * @return {?string} the reason, or null.
     */
    passwordRef = React.createRef();
    redisProbeId = 0;

    componentWillUnmount() { this.redisProbeId++; }

    changeRedis(field, value) {
        this.redisProbeId++;
        this.setState({[field]: value, redisCheck: null});
    }

    async checkRedis() {
        const host = this.state.redis_host.trim();
        const port = Number(this.state.redis_port);
        if (!host) {
            this.redisProbeId++;
            this.setState({redisCheck: null});
            return;
        }
        if (!Number.isInteger(port) || port < 1 || port > 65535) {
            this.redisProbeId++;
            this.setState({redisCheck: {status: "error", message: "The Redis port must be between 1 and 65535."}});
            return;
        }
        const address = `${host}:${port}`;
        if (this.state.redisCheck?.status === "checking" && this.state.redisCheck.address === address) return;
        const id = ++this.redisProbeId;
        this.setState({redisCheck: {status: "checking", address, message: `Checking Redis at ${address}…`}});
        let error;
        try {
            const result = await ControlAPI.asyncMakeAPICall("InitialSetupControl", {
                action: "check-redis", redis_host: host, redis_port: port
            }, true);
            error = result ? ControlAPI.apiError(result) : "The server did not answer the Redis check. Try again.";
        } catch {
            error = "The Redis check could not be completed. Try again.";
        }
        if (id !== this.redisProbeId) return;
        this.setState({redisCheck: {status: error ? "error" : "success", address,
            message: error || `Redis at ${address} is reachable.`}});
    }

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
        if (!this.state.cluster_password) {
            return "The cluster password is required.";
        }
        if (this.state.cluster_password !== this.state.cluster_password_confirmation) {
            return "The two cluster passwords do not match.";
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

        if (this.state.database_type && !this.state.redis_host) {
            return "A shared accounts database needs shared storage. "
                 + "Provide the Redis host or select SQLite for the user database.";
        }

        if (this.state.database_type) {
            if (!this.state.database_host.trim()) {
                return "The user database hostname is required.";
            }
            try {
                if (/[\s/]/.test(this.state.database_host.trim())) {
                    return "Enter a user database hostname, optionally followed by a port.";
                }
                const host = new URL(`http://${this.state.database_host.trim()}`);
                if (host.username || host.password || host.pathname !== "/" || host.search || host.hash) {
                    return "Enter a user database hostname, optionally followed by a port.";
                }
            } catch {
                return "Enter a valid user database hostname, optionally followed by a port.";
            }
            if (!this.state.database_name.trim()) {
                return "The user database name is required.";
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
        if (this.state.busy) return;
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
                cluster_password: this.state.cluster_password,
                node_name: this.state.node_name,
                node_host: this.state.node_host,
                mqtt_port: parseInt(this.state.mqtt_port),
                mqtt_ssl_port: parseInt(this.state.mqtt_ssl_port),
                web_service_port: servicePort,
                redis_host: this.state.redis_host,
                redis_port: this.state.redis_host ? parseInt(this.state.redis_port) : 0,
                database_uri: this.databaseUri()
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

                errorWindow("The configuration was not replaced: " + failure);
                this.setState({busy: false, failed: true, message: ""});
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
        const adminMismatch = this.state.admin_confirmation_checked &&
            this.state.admin_password !== this.state.admin_password_confirmation;
        const clusterMismatch = this.state.cluster_confirmation_checked &&
            this.state.cluster_password !== this.state.cluster_password_confirmation;
        return <form className="inputScreen" noValidate onSubmit={event => {
            event.preventDefault();
            this.submit();
        }}>
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
                    <PasswordInput id="admin_password" name="admin_password" autoComplete="new-password"
                           ref={this.passwordRef}
                           value={this.state.admin_password}
                           onChange={(e) => this.setState({admin_password: e.target.value})}/>
                </div>
                <div className="inputScreenRow">
                    <label htmlFor="admin_password_confirmation" className="inputScreenLabel">
                        Repeat password:
                    </label>
                    <PasswordInput id="admin_password_confirmation" name="admin_password_confirmation" autoComplete="new-password"
                           value={this.state.admin_password_confirmation}
                           onBlur={() => this.setState({admin_confirmation_checked: true})}
                           aria-invalid={adminMismatch}
                           aria-describedby={adminMismatch ? "admin_password_mismatch" : undefined}
                           onChange={(e) => this.setState({admin_password_confirmation: e.target.value})}/>
                    {adminMismatch && <span id="admin_password_mismatch" role="alert" style={{color: "#c62828", marginLeft: 8}}>
                        The two administrator passwords do not match.
                    </span>}
                </div>
            </fieldset>

            <fieldset>
                <legend>Cluster account</legend>
                <p>Set the same cluster password on every node. For a shared user database,
                    enter the cluster password set on the first node. This does not join a cluster;
                    enable Cluster mode on the first node, then use Join cluster for additional nodes.</p>
                <div className="inputScreenRow">
                    <label htmlFor="cluster_password" className="inputScreenLabel">Cluster password:</label>
                    <PasswordInput id="cluster_password" name="cluster_password" autoComplete="new-password"
                           value={this.state.cluster_password}
                           onChange={(e) => this.setState({cluster_password: e.target.value})}/>
                </div>
                <div className="inputScreenRow">
                    <label htmlFor="cluster_password_confirmation" className="inputScreenLabel">Repeat cluster password:</label>
                    <PasswordInput id="cluster_password_confirmation" name="cluster_password_confirmation"
                           autoComplete="new-password" value={this.state.cluster_password_confirmation}
                           onBlur={() => this.setState({cluster_confirmation_checked: true})}
                           aria-invalid={clusterMismatch}
                           aria-describedby={clusterMismatch ? "cluster_password_mismatch" : undefined}
                           onChange={(e) => this.setState({cluster_password_confirmation: e.target.value})}/>
                    {clusterMismatch && <span id="cluster_password_mismatch" role="alert" style={{color: "#c62828", marginLeft: 8}}>
                        The two cluster passwords do not match.
                    </span>}
                </div>
            </fieldset>

            <fieldset>
                <legend>This Server</legend>
                <div className="inputScreenRow">
                    <label htmlFor="node_name" className="inputScreenLabel">Node name:</label>
                    <input id="node_name" name="node_name" type="text" style={{width: '200px'}}
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
                    <input id="redis_host" name="redis_host" type="text"
                           value={this.state.redis_host}
                           onBlur={() => this.checkRedis()}
                           onChange={(e) => this.changeRedis("redis_host", e.target.value)}/>

                    <label htmlFor="redis_port" className="inputScreenLabel"
                           style={{width: '50px'}}>port:</label>
                    <input id="redis_port" name="redis_port" type="number" style={{width: '80px'}}
                           value={this.state.redis_port}
                           onBlur={() => this.checkRedis()}
                           onChange={(e) => this.changeRedis("redis_port", e.target.value)}/>
                    <HelpButton helpKey="persistence.redis_host"/>
                </div>
                {this.state.redisCheck && <p role={this.state.redisCheck.status === "error" ? "alert" : "status"}
                    style={{color: this.state.redisCheck.status === "error" ? "#c62828" : "#666"}}>
                    {this.state.redisCheck.message}
                </p>}
            </fieldset>

            <fieldset>
                <legend>Accounts Database</legend>
                <div className="inputScreenIntro">
                    <p>
                        Optional. A PostgreSQL or MySQL database shared by every node in a
                        cluster. Select SQLite for a standalone server that keeps its accounts
                        in a local database. For a shared database, the administrator password must
                        match the one already in the shared database if it has been set.
                        If the enabled admin account has no password yet, Setup sets the first
                        administrator and cluster passwords together and preserves other users.
                        Submit and restart checks the PostgreSQL or MySQL connection using the
                        database name and credentials below before applying settings. If the
                        connection fails, the current configuration stays unchanged.
                    </p>
                </div>
                <div className="inputScreenRow">
                    <label htmlFor="database_type" className="inputScreenLabel">User Database URI:</label>
                    <span className="initialSetupDatabaseFields">
                        <select id="database_type" name="database_type" aria-label="Database server type"
                                className="inputScreenSelect" value={this.state.database_type}
                                onChange={(e) => this.setState({database_type: e.target.value})}>
                            <option value="">SQLite (local)</option>
                            <option value="postgresql">PostgreSQL</option>
                            <option value="mysql">MySQL</option>
                        </select>
                        <label htmlFor="database_host">host</label>
                        <input id="database_host" name="database_host" type="text" aria-label="Database hostname"
                               placeholder="hostname" disabled={!this.state.database_type}
                               value={this.state.database_host}
                               onChange={(e) => this.setState({database_host: e.target.value})}/>
                        <label htmlFor="database_name">database</label>
                        <input id="database_name" name="database_name" type="text" aria-label="Database name"
                               disabled={!this.state.database_type} value={this.state.database_name}
                               onChange={(e) => this.setState({database_name: e.target.value})}/>
                    </span>
                    <HelpButton helpKey="initial_setup.database_uri"/>
                </div>
                {this.state.database_type && <>
                    <div className="inputScreenRow">
                        <label htmlFor="database_username" className="inputScreenLabel">Database user:</label>
                        <input id="database_username" name="database_username" type="text"
                               autoComplete="off" value={this.state.database_username}
                               onChange={(e) => this.setState({database_username: e.target.value})}/>
                    </div>
                    <div className="inputScreenRow">
                        <label htmlFor="database_password" className="inputScreenLabel">Database password:</label>
                        <input id="database_password" name="database_password" type="password"
                               autoComplete="new-password" value={this.state.database_password}
                               onChange={(e) => this.setState({database_password: e.target.value})}/>
                    </div>
                </>}
            </fieldset>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button type="submit" className="inputScreenSaveButton" style={{width: "180px"}}
                        disabled={this.state.busy}>
                    {this.state.busy ? "Checking connections and applying setup..." : "Submit and restart"}
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
        </form>;
    }
}
