import React from 'react';
import ControlAPI from "../ControlAPI";
import HelpButton from "../components/HelpButton";
import "./InputScreen.css"
import BasicScreen from "./BasicScreen";
import PageTitle from "../components/PageTitle";
import PasswordInput from "../components/PasswordInput";

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
        max_redis_connections: 32,
        max_queued_writes: 0,
        // redis_uri, presented as separate fields
        redis_host: "localhost",
        redis_port: 6379,
        redis_username: "",
        redis_password: "",
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
        return {
            redis_host: result.groups.host,
            redis_port: result.groups.port ? parseInt(result.groups.port) : 6379,
            redis_username: result.groups.username ? result.groups.username : "",
            redis_password: result.groups.password ? result.groups.password : ""
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

        let uri = "redis://";
        if (this.state.redis_username || this.state.redis_password)
        {
            uri += this.state.redis_username;
            if (this.state.redis_password)
            {
                uri += ":" + this.state.redis_password;
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
    savePersistence()
    {
        ControlAPI.setPersistence({
            redis_uri: this.makeRedisUri(),
            enabled: this.state.enabled,
            clean_start: this.state.clean_start,
            max_redis_connections: parseInt(this.state.max_redis_connections),
            max_queued_writes: parseInt(this.state.max_queued_writes)
        });
    }

    componentDidMount()
    {
        ControlAPI.asyncMakeAPICall("PersistenceControl", {action: "get"})
            .then(data =>
            {
                if (!data || !data.persistence)
                {
                    return;
                }
                const persistence = data.persistence;
                const parsedURI = this.parseRedisUri(persistence.redis_uri);
                this.setState({
                    ...persistence,
                    ...parsedURI,
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
                        <legend>Persistence Behaviour</legend>
                        {this.renderCheckBox("Clean start", "clean_start", "persistence.clean_start")}
                        {this.renderInput("Max queued writes", "max_queued_writes",
                                          "persistence.max_queued_writes", "90px")}
                    </fieldset>
                </div>

                <div className="inputScreenRow">
                    <label className="inputScreenLabel"/>
                    <button className="inputScreenSaveButton"
                            onClick={() => this.savePersistence()}>
                        Save
                    </button>
                </div>
            </div>
        );
    }
}
