import React from 'react';
import "./InputScreen.css"
import ControlAPI from "../ControlAPI";
import BasicScreen from "./BasicScreen";
import PageTitle from "../components/PageTitle";
import HelpButton from "../components/HelpButton";
import PasswordInput from "../components/PasswordInput";
import {errorWindow, informationWindow} from "../components/MessageWindow";

/**
 * Prometheus endpoint screen.
 *
 * The fields match the PrometheusSettings type in xmq.wsdl, that is the "prometheus" section of
 * the server configuration. Saving replaces the listener at once, so a port change does not need
 * a broker restart and does not disconnect MQTT clients.
 */
export default class PrometheusScreen extends BasicScreen {
    state = {
        enabled: false,
        bind_ip: "0.0.0.0",
        port: 9184,
        username: "",
        password: "",
        listening: false
    };

    componentDidMount() {
        this.load();
    }

    load() {
        ControlAPI.asyncMakeAPICall("PrometheusControl", {action: "get"})
            .then(result => {
                if (!result || !result.settings) {
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't read the Prometheus settings: " + error);
                    return;
                }
                this.setState({
                    enabled: result.settings.enabled,
                    bind_ip: result.settings.bind_ip,
                    port: result.settings.port,
                    username: result.settings.username || "",
                    password: result.settings.password || "",
                    listening: result.listening
                });
            });
    }

    render() {
        return <div>
            <PageTitle>Prometheus</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Exposes the broker's counters to Prometheus on a listener of its own, at
                    <code> GET /metrics</code>. It is off unless enabled here.
                </p>
                <p>
                    The endpoint is not encrypted. With no username it needs no password either:
                    bind it to an address only the scraper can reach. A username and password make
                    scrapes use HTTP Basic authentication, which Prometheus configures as
                    <code> basic_auth</code>. Each node answers for itself, with host and process
                    figures as well as the broker's own.
                </p>
                {this.state.listening
                    ? <p>Listening on {this.state.bind_ip}:{this.state.port}.</p>
                    : <p>Not listening.</p>}
            </div>

            <fieldset>
                <legend>Endpoint</legend>
                {this.renderCheckBox("Enabled", "enabled", "prometheus.enabled")}
                {this.renderTextInput("Bind address", "bind_ip", "prometheus.bind_ip", "140px")}
                {this.renderInput("Port", "port", "prometheus.port", "90px")}
                {this.renderTextInput("Username", "username", "prometheus.username", "140px")}
                <div className="inputScreenRow">
                    <label htmlFor="prometheus_password" className="inputScreenLabel">Password:</label>
                    <PasswordInput id="prometheus_password" name="prometheus_password"
                                   style={{width: "140px"}}
                                   value={this.state.password}
                                   placeholder="not set"
                                   onChange={e => this.setState({password: e.target.value})}/>
                    <HelpButton helpKey="prometheus.password"/>
                </div>
            </fieldset>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" onClick={() => this.save()}>
                    Save
                </button>
            </div>
        </div>
    }

    save() {
        ControlAPI.asyncMakeAPICall("PrometheusControl", {
            action: "set",
            settings: {
                enabled: this.state.enabled,
                bind_ip: this.state.bind_ip,
                port: parseInt(this.state.port),
                username: this.state.username,
                password: this.state.password
            }
        }).then(result => {
            if (result === null) {
                // XMQ server is offline
                return false;
            }
            const error = ControlAPI.apiError(result);
            if (error) {
                errorWindow("Can't change the Prometheus settings: " + error);
                return false;
            }
            if (result.settings) {
                this.setState({
                    enabled: result.settings.enabled,
                    bind_ip: result.settings.bind_ip,
                    port: result.settings.port,
                    username: result.settings.username || "",
                    password: result.settings.password || "",
                    listening: result.listening
                });
            }
            informationWindow(result.listening
                                  ? "Prometheus is now listening on " + result.settings.bind_ip + ":" + result.settings.port + "."
                                  : "Prometheus settings saved; the endpoint is not listening.",
                              "Prometheus settings");
            return true;
        });
    }
}
