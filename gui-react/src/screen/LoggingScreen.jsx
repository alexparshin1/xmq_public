import React from 'react';
import {errorWindow} from "../components/MessageWindow";
import "./InputScreen.css"
import ControlAPI from "../ControlAPI";
import BasicScreen from "./BasicScreen";
import HelpButton from "../components/HelpButton";
import PageTitle from "../components/PageTitle";

/**
 * Available log levels, quietest first, matching the LogLevel type in xmq.wsdl.
 */
const LogLevels = ["PANIC", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG"];

/**
 * Log subjects, matching the SubjectLogLevels type in xmq.wsdl.
 * Each entry is [field name, label], grouped the way they are shown on the page.
 */
const ServerSubjects = [
    ["server_connections", "Connections"],
    ["server_events", "Other events"]
];

const SessionSubjects = [
    ["connect", "Connect"],
    ["disconnect", "Disconnect"],
    ["subscribe", "Subscribe"],
    ["unsubscribe", "Unsubscribe"],
    ["publish", "Publish"],
    ["ack", "Acks"]
];

// The cluster subjects, cluster_connections and cluster_events, are deliberately not shown:
// clustering isn't exposed in the interface yet. Their levels are still kept in the page state
// and written back unchanged, so a configuration that sets them isn't altered by a save here.

/**
 * Logging screen.
 * The fields match the Logging type in xmq.wsdl, that is the "logging" section of the
 * server configuration: a log file, a level ceiling, and a level per log subject.
 */
export default class LoggingScreen extends BasicScreen {
    state = {
        // Left empty until the server reports it: the log path is the server's, and its shape
        // depends on the server's platform, which the browser has no way of knowing.
        log_to: "",
        min_log_level: "INFO",
        log_level: {
            connect: "INFO",
            disconnect: "INFO",
            subscribe: "INFO",
            unsubscribe: "INFO",
            publish: "ERROR",
            ack: "ERROR",
            server_connections: "INFO",
            server_events: "INFO",
            cluster_connections: "INFO",
            cluster_events: "INFO"
        }
    };

    componentDidMount() {
        ControlAPI.asyncMakeAPICall("LoggingControl", {action: "get"})
            .then(result => {
                if (!result || !result.logging) {
                    return;
                }
                const logging = result.logging;
                this.setState({
                    log_to: logging.log_to,
                    min_log_level: logging.min_log_level,
                    // Keep the defaults for any subject the server didn't report,
                    // so an older configuration file doesn't blank the selects.
                    log_level: {...this.state.log_level, ...logging.log_level}
                });
            });
    }

    setSubjectLevel(subject, level) {
        this.setState({
            log_level: {
                ...this.state.log_level,
                [subject]: level
            }
        });
    }

    renderLevelSelect(label, value, onChange, helpKey = null) {
        return <div className="inputScreenRow" key={label}>
            <label className="inputScreenLabel">{label}:</label>
            <select className="inputScreenSelect" value={value}
                    onChange={(e) => onChange(e.target.value)}>
                {LogLevels.map(level => <option key={level} value={level}>{level}</option>)}
            </select>
            <HelpButton helpKey={helpKey}/>
        </div>
    }

    renderSubjectGroup(legend, subjects, helpKey = null) {
        return <fieldset>
            <legend>{legend}</legend>
            {subjects.map(([subject, label]) =>
                this.renderLevelSelect(label, this.state.log_level[subject],
                                       (level) => this.setSubjectLevel(subject, level),
                                       subject === "publish" ? helpKey : null))}
        </fieldset>
    }

    render() {
        return <div>
            <PageTitle>Logging</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Every log subject has its own level, and the minimum log level below caps all
                    of them: a subject set to DEBUG still logs at INFO while the cap is INFO.
                </p>
                <p>
                    Session errors are always logged at ERROR and are not listed here.
                </p>
            </div>

            <div className="inputScreenRow">
                <label htmlFor="log_to" className="inputScreenLabel">Log to file:</label>
                <input name="log_to" value={this.state.log_to}
                       style={{width: '300px'}}
                       onChange={(e) => this.setState({log_to: e.target.value})}/>
                <HelpButton helpKey="logging.log_to"/>
            </div>

            {this.renderLevelSelect("Minimum log level", this.state.min_log_level,
                                    (level) => this.setState({min_log_level: level}),
                                    "logging.min_log_level")}

            {this.renderSubjectGroup("Server", ServerSubjects)}
            {this.renderSubjectGroup("Session Messages", SessionSubjects, "logging.publish")}

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" onClick={e => this.saveLogging()}>
                    Save
                </button>
            </div>
        </div>
    }

    saveLogging() {
        ControlAPI.asyncMakeAPICall("LoggingControl",
            {
                action: "set",
                logging: {
                    log_to: this.state.log_to,
                    min_log_level: this.state.min_log_level,
                    log_level: this.state.log_level
                }
            })
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return false;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't change the logging settings: " + error);
                    return false;
                }
                return true;
            });
    }
}
