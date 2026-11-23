import React, {useState} from "react";
import ControlAPI from "../ControlAPI";
import "./DialogWindow.css";

export default function WebhookRuleTest({sql, logs, onClose}) {
    const [values, setValues] = useState(() => ({
        ...(logs ? {level: "error", subject: "server_connections", message: ""}
                 : {topic: "sensors/1", payload: "", client_id: "", username: "", qos: "1", retain: "false"}),
        timestamp: new Date().toISOString()
    }));
    const [result, setResult] = useState(null);
    const [error, setError] = useState("");
    const [busy, setBusy] = useState(false);
    const edit = (name, value) => {
        setValues(previous => ({...previous, [name]: value}));
        setResult(null);
        setError("");
    };
    const run = async event => {
        event.preventDefault();
        setBusy(true);
        setResult(null);
        setError("");
        try {
            const response = await ControlAPI.asyncMakeAPICall("ExtensionControl", {
                action: "test_sql",
                settings: Object.entries({...values, sql, source: logs ? "logs" : "mqtt"})
                    .map(([name, value]) => ({name, value}))
            }, true);
            const problem = response ? ControlAPI.apiError(response) : "No response from the broker.";
            if (problem) setError(problem);
            else setResult(JSON.parse(response.message));
        } catch {
            setError("Could not read the test result.");
        } finally {
            setBusy(false);
        }
    };
    const field = (name, label, options) => <label className="webhookRuleTestField">
        {label}
        {options ? <select value={values[name]} onChange={event => edit(name, event.target.value)}>
            {options.map(option => <option key={option}>{option}</option>)}
        </select> : <input value={values[name]} onChange={event => edit(name, event.target.value)}/>}
    </label>;
    const body = logs ? "message" : "payload";
    return <div className="modalBackground">
        <form className="modalContainer webhookRuleTest" role="dialog" aria-modal="true"
              aria-label={logs ? "Test log SQL rules" : "Test MQTT SQL rules"} onSubmit={run}>
            <div className="title">{logs ? "Test log SQL rules" : "Test MQTT SQL rules"}</div>
            <div className="webhookRuleTestBody">
                <p>Uses the current editor contents. Does not save settings or send an HTTP request.</p>
                <fieldset disabled={busy}>
                    {logs ? <>
                        {field("level", "Level", ["debug", "info", "warning", "error"])}
                        {field("subject", "Subject")}
                    </> : <>
                        {field("topic", "Topic")}
                        {field("client_id", "Client ID")}
                        {field("username", "Username")}
                        {field("qos", "QoS", ["0", "1", "2"])}
                        {field("retain", "Retain", ["false", "true"])}
                    </>}
                    {field("timestamp", "Timestamp")}
                    <label className="webhookRuleTestField">
                        {logs ? "Log message" : "Message body (payload)"}
                        <textarea rows={4} value={values[body]}
                                  onChange={event => edit(body, event.target.value)}/>
                    </label>
                </fieldset>
                {error && <p role="alert" className="extensionValueError">{error}</p>}
                {result && <div aria-live="polite">
                    <strong>{result.matched ? `Matched — rule ${result.rule}` : "Not matched"}</strong>
                    {result.invalid_json && <p>Invalid JSON: rules that needed a payload field were skipped.</p>}
                    {result.matched && <table className="mqttSqlHelpFields">
                        <thead><tr><th>Output field</th><th>JSON value</th></tr></thead>
                        <tbody>{Object.entries(result.fields).map(([key, value]) => <tr key={key}>
                            <td><code>{key}</code></td><td><code>{JSON.stringify(value)}</code></td>
                        </tr>)}</tbody>
                    </table>}
                </div>}
            </div>
            <div className="modalFooter">
                <button className="modalOkButton" disabled={busy} type="submit">Run test</button>
                <button className="modalCancelButton" type="button" onClick={onClose}>Close</button>
            </div>
        </form>
    </div>;
}
