import React from 'react';
import "./InputScreen.css"
import ControlAPI from "../ControlAPI";
import BasicScreen from "./BasicScreen";
import PageTitle from "../components/PageTitle";
import {errorWindow, informationWindow} from "../components/MessageWindow";

/**
 * Server and queue limits screen.
 * The fields match the ServerLimits and QueueLimits types in xmq.wsdl, that is the
 * "server_limits" and "queue_limits" sections of the server configuration.
 */
export default class ServerLimitsScreen extends BasicScreen {
    static threadCount(value) {
        return String(value).trim().toLowerCase() === "auto" ? "auto" : parseInt(value);
    }

    state = {
        send_threads: 3,
        receive_threads: 3,
        delivery_threads: "auto",
        max_topic_alias: 128,
        max_packet_size: 256 * 1024 * 1024,
        max_size: 50000,
        max_inflight_messages: 32768
    };

    componentDidMount() {
        ControlAPI.asyncMakeAPICall("LimitsControl", {action: "get"})
            .then(result => {
                if (!result || !result.server_limits || !result.queue_limits) {
                    return;
                }
                this.setState({
                    send_threads: result.server_limits.send_threads,
                    receive_threads: result.server_limits.receive_threads,
                    delivery_threads: result.server_limits.delivery_threads,
                    max_topic_alias: result.server_limits.max_topic_alias,
                    max_packet_size: result.server_limits.max_packet_size,
                    max_size: result.queue_limits.max_size,
                    max_inflight_messages: result.queue_limits.max_inflight_messages
                });
            });
    }

    render() {
        return <div>
            <PageTitle>Server Limits</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Thread counts and the bounds the server places on each client session.
                </p>
                <p>
                    The thread groups on this page share the host's physical cores with the
                    persistence threads and with the per-listener threads
                    set on the Listeners page. No group should be larger than the host's physical
                    core count.
                </p>
            </div>

            <fieldset>
                <legend>Threads</legend>
                {this.renderInput("Message send threads", "send_threads",
                                  "server_limits.send_threads")}
                {this.renderInput("Message receive threads", "receive_threads",
                                  "server_limits.receive_threads")}
                {this.renderInput("Message delivery threads", "delivery_threads",
                                  "server_limits.delivery_threads")}
            </fieldset>

            <fieldset>
                <legend>Session Limits</legend>
                {this.renderInput("Max topic alias", "max_topic_alias",
                                  "server_limits.max_topic_alias", "90px")}
                {this.renderInput("Max queue size", "max_size",
                                  "queue_limits.max_size", "110px")}
                {this.renderInput("Max packet size", "max_packet_size",
                                  "server_limits.max_packet_size", "130px")}
                {this.renderInput("Max inflight messages", "max_inflight_messages",
                                  "queue_limits.max_inflight_messages", "110px")}
            </fieldset>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" onClick={e => this.saveLimits()}>
                    Save
                </button>
            </div>
        </div>
    }

    saveLimits() {
        ControlAPI.asyncMakeAPICall("LimitsControl",
            {
                action: "set",
                server_limits: {
                    send_threads: parseInt(this.state.send_threads),
                    receive_threads: parseInt(this.state.receive_threads),
                    delivery_threads: ServerLimitsScreen.threadCount(this.state.delivery_threads),
                    max_topic_alias: parseInt(this.state.max_topic_alias),
                    max_packet_size: parseInt(this.state.max_packet_size)
                },
                queue_limits: {
                    max_size: parseInt(this.state.max_size),
                    max_inflight_messages: parseInt(this.state.max_inflight_messages)
                }
            }
        ).then(result => {
            if (result === null) {
                // XMQ server is offline
                return false;
            }
            const error = ControlAPI.apiError(result);
            if (error) {
                errorWindow("Can't change the server limits: " + error);
                return false;
            }
            informationWindow("Server limits changed, please restart XMQ to take effect");
            return true;
        });
    }
}
