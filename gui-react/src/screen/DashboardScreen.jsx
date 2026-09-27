import React from 'react';
import "./InputScreen.css"
import ControlAPI from "../ControlAPI";
import BasicScreen from "./BasicScreen";
import StatPanel, {Gauge, Stat, formatBytes, formatCount, formatDuration} from "../components/StatPanel";
import PowerSwitch from "../components/PowerSwitch";
import PageTitle from "../components/PageTitle";
import {confirmationWindow, errorWindow} from "../components/MessageWindow";

/**
 * Dashboard.
 *
 * For now it carries one control: starting and stopping the MQTT server. The interface is served
 * from outside the server, so it stays up while the server is stopped - which is what makes it
 * possible to correct a configuration that prevents the server from starting at all.
 *
 * Intended to grow into the place where the server's vital signs are shown; the layout leaves
 * room for that rather than centring a single button.
 */
export default class DashboardScreen extends BasicScreen {
    state = {
        running: null,      // null until the first status reply: "unknown" is not "stopped"
        version: "",
        message: "",        // why the server is not running, when it was asked to start
        busy: false,
        cleanStart: null,   // null until read: "not known" is not "no", and this one guards a database
        statistics: null,   // last GetStatistics reply
        rates: null,        // messages per second, worked out between polls
        pollSeconds: 10
    }

    componentDidMount() {
        this.refreshStatus();
        this.readStatistics();
        this.startPolling();
    }

    componentWillUnmount() {
        this.stopPolling();
    }

    startPolling() {
        this.stopPolling();
        this.timer = setInterval(() => {
            // Skipped while the tab is in the background: an open dashboard nobody is looking at
            // should not keep asking, nor keep a session alive by doing so.
            if (document.visibilityState === "visible") {
                this.readStatistics();
                this.sendAction("status");
                if (this.state.cleanStart === null) {
                    // Only while it is still unknown: the read on arrival did not get through,
                    // and without it the clean-start warning cannot be offered at all.
                    this.readPersistence();
                }
            }
        }, this.state.pollSeconds * 1000);
    }

    stopPolling() {
        if (this.timer) {
            clearInterval(this.timer);
            this.timer = null;
        }
    }

    setPollSeconds(pollSeconds) {
        this.setState({pollSeconds: pollSeconds}, () => this.startPolling());
    }

    /**
     * Reads the counters, and works out per-second rates from the change since the last read.
     *
     * The server reports totals since it started, which say how much has happened but not how
     * busy it is now. The difference between two reads is what answers that, and it can only be
     * taken here, where the previous read is remembered.
     */
    readStatistics() {
        ControlAPI.asyncMakeAPICall("GetStatistics", {}, true)
            .then(result => {
                if (!result || ControlAPI.apiError(result)) {
                    return;
                }
                const now = Date.now();
                const previous = this.state.statistics;
                let rates = null;
                if (previous && previous.broker && result.broker) {
                    const seconds = (now - previous.readAt) / 1000;
                    // A restart resets the counters; a negative difference means the totals are
                    // not comparable, so no rate is claimed rather than a wrong one shown.
                    const delta = (field) => result.broker[field] - previous.broker[field];
                    if (seconds > 0 && delta("messages_publish_received") >= 0 && delta("messages_publish_sent") >= 0) {
                        rates = {
                            // Publishes, not packets. The all-message counters include the
                            // acknowledgements, so a QoS 1 publish shows up twice in them - once
                            // going out and once as the PUBACK coming back - and the rate reads
                            // as double what was actually published.
                            received: delta("messages_publish_received") / seconds,
                            sent: delta("messages_publish_sent") / seconds,
                            packetsIn: delta("messages_received") / seconds,
                            packetsOut: delta("messages_sent") / seconds,
                            bytesIn: delta("bytes_received") / seconds,
                            bytesOut: delta("bytes_sent") / seconds
                        };
                    }
                }
                result.readAt = now;
                this.setState({statistics: result, rates: rates || this.state.rates});
            });
    }

    refreshStatus() {
        this.sendAction("status");
        this.readPersistence();
    }

    /**
     * Reads whether starting the server would empty the persistent store.
     *
     * Read fresh rather than remembered: persistence can be changed - in another tab, or by
     * another person - between this page loading and Start being pressed.
     *
     * Quietly, because nobody asked for it: this page is where signing in leads, and it makes
     * this read on arrival. A failure here is not worth a window over the screen - it only means
     * the warning below cannot be offered, and the read is made again with the next poll.
     */
    readPersistence() {
        return ControlAPI.asyncMakeAPICall("PersistenceControl", {action: "get"}, true)
            .then(data => {
                if (!data || !data.persistence) {
                    return null;
                }
                const persistence = data.persistence;
                const cleanStart = persistence.enabled === true && persistence.clean_start === true;
                this.setState({cleanStart: cleanStart});
                return cleanStart;
            });
    }

    /**
     * Starts the server, first warning if doing so would discard the stored sessions.
     *
     * Stop keeps persistent sessions; a start with clean_start set then empties the database
     * before restoring anything. Without asking, stop-then-start reads as a pause and quietly
     * loses everything queued for offline clients.
     */
    startServer() {
        // Read now rather than trusted from arrival: this is the one thing standing between a
        // press of Start and an emptied database, and the read on arrival may not have got
        // through - or the setting may have been changed since, here or by somebody else.
        this.readPersistence().then(async cleanStart => {
            if (cleanStart === null) {
                errorWindow("Whether starting the server would empty the database could not be " +
                             "read, so it cannot be said what Start would discard. Try again.");
                return;
            }
            if (cleanStart) {
                const confirmed = await confirmationWindow(
                    "Persistence is enabled with \"clean start\", so starting the server empties the " +
                    "database first.\n\nEvery stored session and every message queued for a client that " +
                    "is currently offline will be discarded.\n\nStart the server anyway?");
                if (!confirmed) {
                    return;
                }
            }
            this.sendAction("start");
        });
    }

    /**
     * Sends a server control action and folds the reply into the state.
     * @param {string} action  One of "status", "start", "stop".
     */
    sendAction(action) {
        this.setState({busy: true});
        // Quiet when it is the timer asking: a missed poll is not news, and the switch shows the
        // state as unknown until one gets through. Starting and stopping are asked for by hand
        // and say so when they fail.
        ControlAPI.asyncMakeAPICall("ServerControl", {action: action}, action === "status")
            .then(result => {
                if (result === null) {
                    // The call did not get through. What the server is doing is now unknown, and
                    // saying so beats leaving the last answer on screen as though it still held.
                    this.setState({busy: false, running: null});
                    return;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    this.setState({busy: false, message: error});
                    return;
                }
                ControlAPI.setServerVersion(result.version);
                this.setState({
                    busy: false,
                    running: result.running === true,
                    version: result.version || this.state.version,
                    // A start that did not happen explains itself here rather than in a popup:
                    // the reason usually needs reading twice and acting on.
                    message: result.message || "",
                    // The counters belong to the server that has just gone or just arrived, so
                    // the rates worked out from them mean nothing across the change.
                    rates: action === "status" ? this.state.rates : null
                });

                if (action !== "status") {
                    // Read again at once rather than waiting for the next poll: otherwise a
                    // stopped server goes on showing its broker figures for up to a minute,
                    // which reads as though the stop did not take.
                    this.readStatistics();
                }
            });
    }

    render() {
        const {running, version, message, busy} = this.state;

        return <div className="InputScreen">
            <PageTitle>Dashboard</PageTitle>

            <fieldset>
                <legend>MQTT Server</legend>

                <div style={{display: "flex", alignItems: "center", gap: "24px", margin: "10px 0"}}>
                    <PowerSwitch running={running}
                                 busy={busy}
                                 onStart={() => this.startServer()}
                                 onStop={() => this.sendAction("stop")}/>
                    {version ? <div style={{color: "#666"}}>version {version}</div> : null}
                </div>

                {message
                    ? <div style={{color: "#c62828", margin: "8px 0", whiteSpace: "pre-wrap"}}>{message}</div>
                    : null}

                {!running && this.state.cleanStart === true
                    ? <div style={{color: "#b26a00", margin: "8px 0", maxWidth: "46em"}}>
                        Persistence is set to <b>clean start</b>: starting the server will empty the
                        database, discarding stored sessions and anything queued for offline clients.
                    </div>
                    : null}

                <p style={{color: "#666", fontSize: "0.9em", maxWidth: "46em"}}>
                    Stopping the server closes every MQTT listener and disconnects its clients; this
                    interface stays available. Configuration can be changed while the server is
                    stopped &mdash; it is saved straight away and applied when the server is
                    started again.
                </p>
            </fieldset>

            {this.renderPanels()}
        </div>;
    }

    renderPanels() {
        const {statistics, rates, pollSeconds} = this.state;
        if (!statistics) {
            return <p style={{color: "#888"}}>Reading statistics&hellip;</p>;
        }

        const broker = statistics.broker;
        const host = statistics.host;
        const redis = statistics.redis;

        return <>
            <div style={{display: "flex", alignItems: "center", gap: "8px", margin: "14px 0 6px"}}>
                <span style={{fontWeight: "bold"}}>Statistics</span>
                <span style={{color: "#888", fontSize: "0.85em"}}>refreshed every</span>
                <select value={pollSeconds}
                        onChange={event => this.setPollSeconds(parseInt(event.target.value))}>
                    <option value={10}>10 seconds</option>
                    <option value={30}>30 seconds</option>
                    <option value={60}>1 minute</option>
                </select>
            </div>

            <div style={{display: "flex", flexWrap: "wrap", gap: "12px"}}>
                <StatPanel title="Broker"
                           note={broker ? null : "The server is stopped, so it holds no counters."}>
                    {broker
                        ? <>
                            <Stat label="Clients connected" value={formatCount(broker.clients_connected)}/>
                            <Stat label="Peak clients" value={formatCount(broker.clients_maximum)}/>
                            <Stat label="Topics" value={formatCount(broker.subscriptions)}
                                  hint="Topics the server knows, including its own $SYS topics"/>
                            <Stat label="Retained messages" value={formatCount(broker.messages_retained)}/>
                            <Stat label="Stored messages" value={formatCount(broker.messages_stored)}/>
                            <Stat label="Dropped publishes" value={formatCount(broker.messages_publish_dropped)}/>
                            <Stat label="Serving for" value={formatDuration(broker.uptime_seconds)}/>
                        </>
                        : <Stat label="Clients connected" value="—"/>}
                </StatPanel>

                <StatPanel title="Queues"
                           note={broker
                                 ? "Work waiting inside the broker, sampled once a second. Counters above say what it has done; these say whether it is keeping up."
                                 : "The server is stopped."}>
                    {broker
                        ? <>
                            <Stat label="Receive" value={formatCount(broker.queue_receive)}
                                  hint="Sessions handed over by the reactor and not yet read"/>
                            <Stat label="Send" value={formatCount(broker.queue_send)}
                                  hint="Sessions with messages waiting to go out"/>
                            <Stat label="Delivery" value={formatCount(broker.queue_delivery)}
                                  hint="Fan-out tasks waiting for a delivery worker"/>
                        </>
                        : <Stat label="Receive" value="—"/>}
                </StatPanel>

                <StatPanel title="Throughput"
                           note={rates ? null : "Rates appear once two readings have been taken."}>
                    {broker
                        ? <>
                            <Stat label="Publishes in"
                                  value={rates ? `${rates.received.toFixed(1)}/s` : "—"}
                                  hint="Worked out from the change between readings"/>
                            <Stat label="Publishes out"
                                  value={rates ? `${rates.sent.toFixed(1)}/s` : "—"}/>
                            <Stat label="Packets in"
                                  value={rates ? `${rates.packetsIn.toFixed(1)}/s` : "—"}
                                  hint="Every MQTT packet, acknowledgements included"/>
                            <Stat label="Packets out"
                                  value={rates ? `${rates.packetsOut.toFixed(1)}/s` : "—"}/>
                            <Stat label="Bytes in"
                                  value={rates ? `${formatBytes(rates.bytesIn)}/s` : "—"}/>
                            <Stat label="Bytes out"
                                  value={rates ? `${formatBytes(rates.bytesOut)}/s` : "—"}/>
                            <Stat label="Total publishes in" value={formatCount(broker.messages_publish_received)}/>
                            <Stat label="Total publishes out" value={formatCount(broker.messages_publish_sent)}/>
                        </>
                        : <Stat label="Messages in" value="—"/>}
                </StatPanel>

                <StatPanel title="Host">
                    {host
                        ? <>
                            <Gauge fraction={host.cpu_percent_total / 100}
                                   caption="CPU, whole machine"
                                   detail={`${host.cpu_percent_total.toFixed(1)}%`}/>
                            {/* Drawn on the same scale as the machine above - a share of the
                                whole host - so the two bars can be read against each other. The
                                figure beside it stays in the familiar form, where 400% means
                                four cores' worth, as top reports it.

                                Without a core count - an older server, which does not report one -
                                the bar falls back to a share of a single core. That over-reads on
                                a multi-core host, but a bar that moves and overstates is worth
                                more than one that sits at zero looking broken. */}
                            <Gauge fraction={host.cpu_percent_process /
                                             (100 * (host.cpu_cores || 1))}
                                   caption="CPU, XMQ"
                                   detail={host.cpu_cores
                                               ? `${host.cpu_percent_process.toFixed(1)}% of ${host.cpu_cores} cores`
                                               : `${host.cpu_percent_process.toFixed(1)}% (core count unknown)`}/>
                            <Gauge fraction={host.memory_total ? host.memory_used / host.memory_total : 0}
                                   caption="Memory in use"
                                   detail={`${formatBytes(host.memory_used)} of ${formatBytes(host.memory_total)}`}/>
                            <Stat label="Memory available"
                                  value={formatBytes(host.memory_available)}
                                  hint="What a new workload could claim, including reclaimable cache"/>
                            <Stat label="Memory, XMQ" value={formatBytes(host.memory_used_by_process)}/>
                            <Stat label="Process up" value={formatDuration(host.process_uptime_seconds)}/>
                        </>
                        : <Stat label="CPU" value="—"/>}
                </StatPanel>

                {redis
                    ? <StatPanel title="Redis"
                                 note={redis.is_local === false
                                           ? "Disk is not shown: Redis is on another host, where this machine's figures would not apply."
                                           : (redis.disk_total ? null : "Redis did not report where its database lives, so disk is not shown.")}>
                        <Stat label="Connection"
                              value={redis.connected ? "Connected" : "Not connected"}/>
                        <Stat label="Host" value={redis.host || "—"}/>
                        <Stat label="Keys" value={formatCount(redis.keys)}/>
                        <Stat label="Memory in use" value={formatBytes(redis.used_memory)}/>
                        <Stat label="Memory peak" value={formatBytes(redis.used_memory_peak)}/>
                        {redis.disk_total
                            ? <Gauge fraction={1 - redis.disk_available / redis.disk_total}
                                     caption="Database filesystem"
                                     detail={`${formatBytes(redis.disk_available)} free of ${formatBytes(redis.disk_total)}`}/>
                            : null}
                    </StatPanel>
                    : null}
            </div>
        </>;
    }
}
