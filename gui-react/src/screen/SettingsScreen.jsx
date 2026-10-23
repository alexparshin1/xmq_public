import React from "react";
import ControlAPI from "../ControlAPI";
import PageTitle from "../components/PageTitle";
import PasswordInput from "../components/PasswordInput";
import {confirmationWindow} from "../components/MessageWindow";
import "./InputScreen.css";

const decode = value => decodeURIComponent(value || "");

export default class SettingsScreen extends React.Component {
    state = {values: null, busy: false, pending: false, message: "", error: "", interfaceUrl: ""};
    unmounted = false;

    componentDidMount() { this.read(); }
    componentWillUnmount() { this.unmounted = true; clearInterval(this.poll); }

    valuesFrom(settings) {
        const redis = settings.redis_uri ? new URL(settings.redis_uri) : null;
        const shared = settings.database_uri && !settings.database_uri.startsWith("sqlite");
        const database = shared ? new URL(settings.database_uri) : null;
        return {...settings,
            redis_host: redis?.hostname || "", redis_port: redis?.port || 6379,
            redis_user: decode(redis?.username), redis_password: decode(redis?.password),
            database_type: database ? database.protocol.slice(0, -1) : "sqlite",
            database_host: database?.hostname || "", database_port: database?.port || (database?.protocol === "mysql:" ? 3306 : 5432),
            database_name: database ? decode(database.pathname.slice(1)) : "",
            database_user: decode(database?.username), database_password: decode(database?.password)
        };
    }

    async read(polling = false) {
        const result = await ControlAPI.asyncMakeAPICall("SettingsControl", {action: "get"}, polling);
        if (this.unmounted) return;
        if (!result) {
            if (polling) {
                clearInterval(this.poll);
                this.setState({pending: false, message: "The interface may have moved or be restarting. Open the interface link below, or reload this page."});
            } else this.setState({error: "Could not read node settings."});
            return;
        }
        const error = ControlAPI.apiError(result);
        if (error) { this.setState({error, pending: false}); clearInterval(this.poll); return; }
        if (polling && !result.pending) {
            clearInterval(this.poll);
            const failed = (result.message || "").startsWith("Settings were not applied");
            this.setState({pending: false, error: failed ? result.message : "", message: failed ? "" : result.message,
                ...(!failed ? {values: this.valuesFrom(result.settings)} : {})});
        } else if (!polling) {
            this.setState({values: this.valuesFrom(result.settings), pending: !!result.pending, message: result.message || ""});
            if (result.pending) this.poll = setInterval(() => this.read(true), 1500);
        }
    }

    change = (field, value) => this.setState(state => ({values: {...state.values, [field]: value}, message: "", error: ""}));

    payload() {
        const v = this.state.values;
        for (const port of [v.mqtt_port, v.mqtt_ssl_port, v.web_service_port,
            ...(v.redis_host.trim() ? [v.redis_port] : []), ...(v.database_type !== "sqlite" ? [v.database_port] : [])]) {
            if (!Number.isInteger(Number(port)) || Number(port) < 1 || Number(port) > 65535)
                throw new Error("Ports must be whole numbers between 1 and 65535.");
        }
        const credentials = (user, password) => user || password ? `${encodeURIComponent(user)}:${encodeURIComponent(password)}@` : "";
        if (v.database_type !== "sqlite" && (!v.database_host.trim() || !v.database_name.trim()))
            throw new Error("Enter the user database host and database name.");
        return {
            node_name: v.node_name, node_host: v.node_host,
            mqtt_port: Number(v.mqtt_port), mqtt_ssl_port: Number(v.mqtt_ssl_port), web_service_port: Number(v.web_service_port),
            redis_uri: v.redis_host.trim() ? `redis://${credentials(v.redis_user, v.redis_password)}${v.redis_host.trim()}:${v.redis_port}` : "",
            database_uri: v.database_type === "sqlite" ? (v.database_uri?.startsWith("sqlite") ? v.database_uri : "")
                : `${v.database_type}://${credentials(v.database_user, v.database_password)}${v.database_host.trim()}:${v.database_port}/${encodeURIComponent(v.database_name.trim())}`
        };
    }

    async request(action) {
        if (this.state.busy || this.state.pending) return;
        let settings;
        try { settings = this.payload(); }
        catch (error) { this.setState({error: error.message}); return; }
        if (action === "set" && !await confirmationWindow(
            "Save these settings and restart MQTT? Clients will disconnect and reconnect. Other settings and accounts will be preserved." +
            (this.state.values.clean_start && settings.redis_uri
                ? "\n\nClean start is enabled: this node's persisted sessions and queued messages will be discarded on restart. Other nodes' data is preserved." : ""))) return;
        this.setState({busy: true, error: "", message: "Checking connections…"});
        const result = await ControlAPI.asyncMakeAPICall("SettingsControl", {action, settings});
        if (this.unmounted) return;
        const error = result ? ControlAPI.apiError(result) : "The interface did not answer. No successful save was confirmed.";
        if (error) { this.setState({busy: false, error, message: ""}); return; }
        const interfaceUrl = new URL(window.location.href);
        interfaceUrl.port = settings.web_service_port;
        interfaceUrl.pathname = "/settings";
        interfaceUrl.search = "";
        interfaceUrl.hash = "";
        this.setState({busy: false, pending: !!result.pending, message: result.message || "Settings queued.",
            interfaceUrl: action === "set" ? interfaceUrl.toString() : ""});
        if (action === "set") {
            clearInterval(this.poll);
            this.poll = setInterval(() => this.read(true), 1500);
        }
    }

    field(label, name, type = "text", extra = {}) {
        const v = this.state.values;
        const props = {id: name, name, value: v[name] ?? "", onChange: e => this.change(name, e.target.value), disabled: this.state.busy || this.state.pending, ...extra};
        return <div className="inputScreenRow" key={name}>
            <label className="inputScreenLabel" htmlFor={name}>{label}:</label>
            {type === "password" ? <PasswordInput {...props} autoComplete="new-password"/> : <input {...props} type={type}/>}
        </div>;
    }

    render() {
        const {values: v, busy, pending, error, message, interfaceUrl} = this.state;
        return <form className="inputScreen" noValidate onSubmit={event => { event.preventDefault(); this.request("set"); }}>
            <PageTitle>Settings</PageTitle>
            <p>Edit this node's connections and ports. Accounts, cluster membership, certificates and other settings are preserved.
                Stored passwords are masked; leave the mask unchanged to retain a password.</p>
            {error && <p role="alert" style={{color: "#c62828"}}>{error}</p>}
            {message && <p role="status">{message}</p>}
            {interfaceUrl && <p>Configuration interface: <a href={interfaceUrl}>{interfaceUrl}</a></p>}
            {!v ? <p>Reading settings…</p> : <>
                <fieldset><legend>This Node</legend>
                    {this.field("Node name", "node_name")}{this.field("Node address", "node_host")}
                    <p className="inputScreenHint">The address is advertised to other nodes. Update its certificate on SSL Keys if the new address is not covered.</p>
                </fieldset>
                <fieldset><legend>Ports</legend>
                    {this.field("MQTT port", "mqtt_port", "number")}
                    {this.field("MQTT+SSL port", "mqtt_ssl_port", "number")}
                    {this.field("Web interface port", "web_service_port", "number")}
                </fieldset>
                <fieldset><legend>Redis</legend>
                    {this.field("Redis host", "redis_host")}{this.field("Redis port", "redis_port", "number")}
                    {this.field("Redis user", "redis_user")}{this.field("Redis password", "redis_password", "password")}
                    <p className="inputScreenHint">Leave host empty to disable persistence. Changing storage does not migrate sessions or queues.
                        A clustered node must keep its current membership; leave the cluster before moving to different storage.</p>
                </fieldset>
                <fieldset><legend>User Database</legend>
                    <div className="inputScreenRow"><label className="inputScreenLabel" htmlFor="database_type">Database type:</label>
                        <select id="database_type" value={v.database_type} disabled={busy || pending}
                            onChange={e => this.setState({values: {...v, database_type: e.target.value, database_port: e.target.value === "mysql" ? 3306 : 5432}, message: "", error: ""})}>
                            <option value="sqlite">Local SQLite</option><option value="postgresql">PostgreSQL</option><option value="mysql">MySQL</option>
                        </select></div>
                    {v.database_type !== "sqlite" && <>
                        {this.field("Database host", "database_host")}{this.field("Database port", "database_port", "number")}
                        {this.field("Database name", "database_name")}{this.field("Database user", "database_user")}
                        {this.field("Database password", "database_password", "password")}
                    </>}
                    <p className="inputScreenHint">The target database must already contain your administrator account with the same password.
                        Accounts are not copied or reset. Use Initial Setup to initialize an empty database.</p>
                </fieldset>
                <button type="button" className="inputScreenSaveButton" disabled={busy || pending} onClick={() => this.request("test")}>Test connections</button>
                {" "}<button type="submit" className="inputScreenSaveButton" disabled={busy || pending}>
                    {pending ? "Applying…" : busy ? "Checking…" : "Save and restart MQTT"}
                </button>
            </>}
        </form>;
    }
}
