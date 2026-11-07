import React from 'react';
import ControlAPI from "../ControlAPI";
import HelpButton from "../components/HelpButton";
import "./InputScreen.css"
import BasicScreen from "./BasicScreen";
import PageTitle from "../components/PageTitle";
import PasswordInput from "../components/PasswordInput";
import {confirmationWindow, errorWindow, informationWindow} from "../components/MessageWindow";

/**
 * Persistence settings screen.
 * The persistence layer is backed by Redis. The fields match the Persistence type
 * in xmq.wsdl, that is the "persistence" section of the server configuration.
 */
export default class PersistenceScreen extends BasicScreen
{
    state = {
        enabled: false,
        clean_start: false,
        max_redis_connections: 2,
        max_queued_writes: 1000,
        mode: "persistent",
        loaded: false,
        busy: false,
        statusMessage: "",
        // redis_uri, presented as separate fields
        redis_host: "localhost",
        redis_port: 6379,
        redis_username: "",
        redis_password: "",
        // Baseline values let an unchanged URI (including legacy credentials) pass through
        // byte-for-byte. The server masks passwords before returning this URI.
        redis_uri_initial: null,
        // Set when redis_uri isn't in the form the fields above can represent.
        // The URI is then edited as text, to avoid rewriting it into something else.
        redis_uri: "",
        redis_uri_is_custom: false
    };

    /**
     * Returns a style object that hides the Redis parameters while persistence is disabled.
     * @return {Object} style object
     */
    displayPersistenceParameters()
    {
        return this.state.enabled ? {display: 'block'} : {display: 'none'};
    }

    /**
     * Parses redis_uri into the host, port, username, and password fields.
     * The supported form is redis://[[username][:password]@]host[:port].
     * @param {String} uri       Redis URI from the server configuration
     * @return {Object} the parsed fields, or null if the URI isn't in the supported form
     */
    parseRedisUri(uri)
    {
        if (!uri)
        {
            return null;
        }
        const matcher = /^redis:\/\/((?<username>[^:@/]*)(:(?<password>[^@]*))?@)?(?<host>[\w.-]+)(:(?<port>\d+))?\/?$/;
        const result = matcher.exec(uri);
        if (!result)
        {
            return null;
        }
        // Older versions wrote Redis credentials into the URI without percent-encoding them.
        // Decode current canonical URIs, but preserve legacy values that contain malformed
        // percent sequences instead of throwing and making the entire screen unusable.
        const decodeCredential = value => {
            if (!value) return "";
            try
            {
                return decodeURIComponent(value);
            }
            catch
            {
                return value;
            }
        };
        return {
            redis_host: result.groups.host,
            redis_port: result.groups.port ? parseInt(result.groups.port) : 6379,
            redis_username: decodeCredential(result.groups.username),
            redis_password: decodeCredential(result.groups.password),
            redis_username_uri: result.groups.username || ""
        };
    }

    /**
     * Builds redis_uri from the host, port, and password fields.
     * A URI that couldn't be parsed on load is passed through unchanged.
     * @return {String} Redis URI
     */
    makeRedisUri()
    {
        if (this.state.redis_uri_is_custom)
        {
            return this.state.redis_uri;
        }

        const initial = this.state.redis_uri_initial;
        if (initial && this.state.redis_host === initial.redis_host &&
            Number(this.state.redis_port) === Number(initial.redis_port) &&
            this.state.redis_username === initial.redis_username &&
            this.state.redis_password === initial.redis_password)
        {
            // In particular, keep the password mask literal so the server can restore the
            // stored secret. This also preserves legacy raw credentials containing '%' or '@'.
            return this.state.redis_uri;
        }

        let uri = "redis://";
        if (this.state.redis_username || this.state.redis_password)
        {
            const username = initial && this.state.redis_username === initial.redis_username
                ? initial.redis_username_uri : encodeURIComponent(this.state.redis_username || "default");
            uri += username || "default";
            if (this.state.redis_password)
            {
                // A masked password is a server-side placeholder, not a credential to encode.
                const password = initial && this.state.redis_password === initial.redis_password &&
                    this.state.redis_password === "*****" ? "*****" : encodeURIComponent(this.state.redis_password);
                uri += ":" + password;
            }
            uri += "@";
        }
        uri += this.state.redis_host;
        if (this.state.redis_port)
        {
            uri += ":" + this.state.redis_port;
        }
        return uri;
    }

    /**
     * Saves the persistence settings, sending the fields defined by the Persistence type.
     */
    async savePersistence()
    {
        const durable = this.state.enabled && this.state.mode === "durable";
        const persistence = {
            redis_uri: this.makeRedisUri(),
            enabled: this.state.enabled,
            clean_start: this.state.clean_start,
            max_redis_connections: parseInt(this.state.max_redis_connections),
            max_queued_writes: durable ? 0 : Math.max(1, parseInt(this.state.max_queued_writes) || 1024),
            ack_after_store: durable
        };
        const explanation = !this.state.enabled
            ? "MQTT clients will disconnect while XMQ restarts with Redis persistence disabled. The configured " +
              "Redis data will be left untouched; sessions in it will not be used until persistence is enabled again."
            : durable
            ? "This changes the Redis configuration for the entire Redis server, affecting every database and " +
              "application using that server. MQTT clients will disconnect while XMQ restarts. Each publish acknowledgement will wait " +
              "for Redis to confirm a local AOF fsync. XMQ will enable Redis appendonly and set " +
              "appendfsync=always. Redis accepts the change without a Redis restart; its config file must " +
              "be writable so XMQ can save the setting. Stored sessions and queued messages will be kept."
            : "This changes the Redis configuration for the entire Redis server, affecting every database and " +
              "application using that server. MQTT clients will disconnect while XMQ restarts. XMQ will write persistent delivery records " +
              "asynchronously with a 10 ms write-behind window. A publisher may receive its acknowledgement " +
              "before Redis stores a record; the queued-write limit bounds that broker-side window. XMQ will " +
              "disable Redis appendonly so AOF does not add write latency, and save the setting in Redis " +
              "configuration. Recovery after a Redis restart then depends on its RDB snapshots; without RDB, " +
              "Redis restart loses its data. Stored sessions and queued messages survive an XMQ restart while Redis remains up.";
        if (!await confirmationWindow(explanation, "Apply persistence mode and restart MQTT")) return;

        this.setState({busy: true, statusMessage: "Checking Redis and restarting MQTT…"});
        try
        {
            let response = await ControlAPI.asyncMakeAPICall("PersistenceControl", {action: "apply", persistence});
            if (!response)
            {
                await errorWindow("The XMQ interface did not answer the persistence request.");
                return;
            }
            const requestError = ControlAPI.apiError(response);
            if (requestError)
            {
                await errorWindow("Persistence mode was not changed: " + requestError);
                return;
            }
            for (let attempt = 0; response.pending === true && attempt < 600; ++attempt)
            {
                await new Promise(resolve => window.setTimeout(resolve, 1000));
                const latest = await ControlAPI.asyncMakeAPICall("PersistenceControl", {action: "get"}, true);
                if (!latest) continue;
                response = latest;
                const error = ControlAPI.apiError(latest);
                if (error) throw new Error(error);
            }
            if (response.pending === true)
                throw new Error("The restart is still in progress. Check the Dashboard before trying again.");
            const message = response.message || "Persistence settings saved.";
            this.setState({statusMessage: message});
            if (message.startsWith("Persistence mode was not applied"))
                await errorWindow(message);
            else
            {
                if (response.persistence)
                    this.setState({
                        ...response.persistence,
                        mode: response.persistence.ack_after_store === true ? "durable" : "persistent"
                    });
                await informationWindow(message);
            }
        }
        catch (error)
        {
            await errorWindow("Persistence mode could not be applied: " + error.message);
        }
        finally
        {
            this.setState({busy: false});
        }
    }

    componentDidMount()
    {
        ControlAPI.asyncMakeAPICall("PersistenceControl", {action: "get"})
            .then(data =>
            {
                if (!data || !data.persistence)
                {
                    this.setState({loaded: true});
                    return;
                }
                const persistence = data.persistence;
                const parsedURI = this.parseRedisUri(persistence.redis_uri);
                this.setState({
                    ...persistence,
                    ...parsedURI,
                    mode: persistence.ack_after_store === true ? "durable" : "persistent",
                    loaded: true,
                    redis_uri_initial: parsedURI,
                    redis_uri_is_custom: parsedURI === null
                });
            });
    }

    /**
     * Renders the Redis server address, either as separate fields, or, when the
     * configured URI can't be represented by them, as the URI itself.
     */
    renderRedisAddress()
    {
        if (this.state.redis_uri_is_custom)
        {
            return <div className="inputScreenRow">
                <label htmlFor="redis_uri" className="inputScreenLabel">Redis URI:</label>
                <input name="redis_uri" type="text" style={{width: '305px'}}
                       value={this.state.redis_uri}
                       onChange={(e) => this.setState({redis_uri: e.target.value})}/>
            </div>;
        }

        return <div>
            <div className="inputScreenRow">
                <label htmlFor="redis_host" className="inputScreenLabel">Redis host:</label>
                <input name="redis_host" type="text" style={{width: '200px'}}
                       value={this.state.redis_host}
                       onChange={(e) => this.setState({redis_host: e.target.value})}/>

                <label htmlFor="redis_port" className="inputScreenLabel"
                       style={{width: '50px'}}>port:</label>
                <input name="redis_port" type="number" style={{width: '80px'}}
                       value={this.state.redis_port}
                       onChange={(e) => this.setState({redis_port: e.target.value})}/>
                <HelpButton helpKey="persistence.redis_host"/>
            </div>

            <div className="inputScreenRow">
                <label htmlFor="redis_username" className="inputScreenLabel">Username:</label>
                <input name="redis_username" type="text" style={{width: '200px'}}
                       value={this.state.redis_username}
                       onChange={(e) => this.setState({redis_username: e.target.value})}/>
                <HelpButton helpKey="persistence.redis_username"/>
            </div>

            <div className="inputScreenRow">
                <label htmlFor="redis_password" className="inputScreenLabel">Password:</label>
                <PasswordInput name="redis_password" style={{width: '200px'}}
                               value={this.state.redis_password}
                               onChange={(e) => this.setState({redis_password: e.target.value})}/>
            </div>
        </div>;
    }

    render()
    {
        return (
            <div>
                <PageTitle>Persistence</PageTitle>

                <div className="inputScreenIntro">
                    <p>
                        XMQ keeps its durable state - client sessions, their subscriptions, and
                        undelivered QoS 1 and QoS 2 messages - in Redis, so that they survive a
                        server restart.
                    </p>
                    <p>
                        Redis is a separate service and is not part of XMQ: install it and start it
                        yourself. It can run on the same host as XMQ, or on a host of its own.
                    </p>
                    <p>
                        If XMQ cannot reach Redis at startup, it logs the error and keeps running
                        with in-memory storage. The server stays up, but nothing is persisted, so
                        it is worth checking the log after enabling persistence.
                    </p>
                </div>

                {this.renderCheckBox("Enabled", "enabled", "persistence.enabled")}

                <div style={this.displayPersistenceParameters()}>
                    <fieldset>
                        <legend>Redis Server</legend>
                        {this.renderRedisAddress()}
                        {this.renderInput("Max Redis connections", "max_redis_connections",
                                          "persistence.max_redis_connections")}
                    </fieldset>

                    <fieldset>
                        <legend>Persistence mode</legend>
                        <div className="inputScreenRow">
                            <label className="inputScreenLabel" htmlFor="persistence_mode">Mode:</label>
                            <select id="persistence_mode" value={this.state.mode} disabled={this.state.busy}
                                    onChange={event => this.setState({
                                        mode: event.target.value,
                                        max_queued_writes: event.target.value === "persistent" &&
                                            (!Number.isFinite(parseInt(this.state.max_queued_writes)) ||
                                             parseInt(this.state.max_queued_writes) <= 0) ? 1000 : this.state.max_queued_writes
                                    })}>
                                <option value="persistent">Persistent — 10 ms write-behind, AOF off</option>
                                <option value="durable">Durable — wait for Redis AOF fsync</option>
                            </select>
                        </div>
                        {this.state.mode === "persistent" && <>
                            <p className="inputScreenHint">Mode changes reconfigure the entire Redis server, affecting every database and application that uses it. XMQ disables Redis AOF for this mode. Redis must permit CONFIG SET and CONFIG REWRITE. A full queued-write window pauses publishers. After a Redis restart, recovery depends on Redis RDB snapshots; with RDB disabled, Redis loses its data.</p>
                            {this.renderInput("Max queued writes", "max_queued_writes",
                                              "persistence.max_queued_writes", "90px")}
                        </>}
                        {this.state.mode === "durable" &&
                            <p className="inputScreenHint">XMQ enables Redis AOF with appendfsync=always, then verifies WAITAOF. Redis must permit CONFIG SET and CONFIG REWRITE; Redis itself does not restart. Durable mode is standalone only.</p>}
                    </fieldset>

                    <fieldset>
                        <legend>Other persistence settings</legend>
                        {this.renderCheckBox("Clean start", "clean_start", "persistence.clean_start")}
                    </fieldset>
                </div>

                <div className="inputScreenRow">
                    <label className="inputScreenLabel"/>
                    <button className="inputScreenSaveButton" disabled={this.state.busy || !this.state.loaded}
                            onClick={() => this.savePersistence()}>
                        {this.state.busy ? "Applying…" : !this.state.loaded ? "Loading…" : "Apply mode and restart MQTT"}
                    </button>
                </div>
                {this.state.statusMessage && <p role="status">{this.state.statusMessage}</p>}
            </div>
        );
    }
}
