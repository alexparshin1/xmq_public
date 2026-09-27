import React from "react";

/**
 * A rocker switch for starting and stopping the server.
 *
 * Modelled on a household light switch, which is the right metaphor: it shows the current state
 * and the way to change it in one object, and it is unambiguous at a glance from across a room.
 * The proportions, the grey face and the indicator's position come from the mockup.
 *
 * The two halves are separately clickable rather than the whole thing toggling. On a switch that
 * toggles, a click always changes something, so a mis-click on a running broker stops it; here
 * pressing the half that is already down does nothing, which is what a real rocker does too.
 *
 * The lamp carries the state: green while serving, red while stopped, amber while a start or stop
 * is in flight - that last one matters more than it sounds, because a start can take seconds when
 * Redis is restoring sessions, and an unlit switch in that gap looks broken.
 */
export default class PowerSwitch extends React.Component {
    render() {
        // running: true, false, or null while the first status reply is outstanding.
        const {running, busy, onStart, onStop} = this.props;

        const unknown = running === null || running === undefined;
        const topPressed = running === true;
        const bottomPressed = running === false;

        const lampColour = busy || unknown ? "#e6a100" : (running ? "#18a300" : "#a70000");
        const lampGlow = busy || unknown ? "rgba(230,161,0,0.65)"
                                         : (running ? "rgba(24,163,0,0.7)" : "rgba(167,0,0,0.6)");

        // A pressed half sits back: darker, with the shadow cast from inside its top edge. A
        // raised half catches the light along its outer edge instead.
        const half = (pressed, extra) => ({
            height: "50%",
            background: pressed
                            ? "linear-gradient(180deg, #b9b9b9 0%, #cfcfcf 100%)"
                            : "linear-gradient(180deg, #e2e2e2 0%, #cbcbcb 100%)",
            boxShadow: pressed
                           ? "inset 0 3px 5px rgba(0,0,0,0.35)"
                           : "inset 0 1px 0 rgba(255,255,255,0.85)",
            cursor: busy ? "wait" : "pointer",
            position: "relative",
            transition: "background 0.15s ease, box-shadow 0.15s ease",
            ...extra
        });

        const press = (action) => {
            if (busy) {
                return;
            }
            action();
        };

        return <div style={{display: "flex", alignItems: "center", gap: "14px"}}>
            <div role="switch"
                 aria-checked={running === true}
                 aria-label="MQTT server"
                 aria-busy={busy ? "true" : "false"}
                 tabIndex={0}
                 onKeyDown={event => {
                     // Space and Enter act on the half that is currently up, so the keyboard does
                     // what the mouse does rather than blindly toggling.
                     if (event.key === " " || event.key === "Enter") {
                         event.preventDefault();
                         press(running ? onStop : onStart);
                     }
                 }}
                 style={{
                     width: "56px",
                     height: "60px",
                     borderRadius: "5px",
                     border: "1px solid #9a9a9a",
                     background: "#aeaeae",
                     padding: "2px",
                     boxShadow: "0 1px 2px rgba(0,0,0,0.3)",
                     opacity: busy ? 0.75 : 1,
                     userSelect: "none"
                 }}>
                <div style={{
                    height: "100%",
                    borderRadius: "3px",
                    overflow: "hidden",
                    display: "flex",
                    flexDirection: "column"
                }}>
                    <div title="Start the server"
                         onClick={() => press(onStart)}
                         style={half(topPressed, {borderBottom: "1px solid #9a9a9a"})}/>
                    <div title="Stop the server"
                         onClick={() => press(onStop)}
                         style={half(bottomPressed)}>
                        {/* Positioned as in the mockup: centred across, three quarters down. */}
                        <div style={{
                            position: "absolute",
                            left: "50%",
                            top: "52%",
                            transform: "translate(-50%, -50%)",
                            width: "9px",
                            height: "5px",
                            borderRadius: "1px",
                            background: lampColour,
                            boxShadow: `0 0 5px 1px ${lampGlow}`,
                            transition: "background 0.2s ease, box-shadow 0.2s ease"
                        }}/>
                    </div>
                </div>
            </div>

            <div>
                <div style={{fontWeight: "bold"}}>
                    {unknown
                        ? <span style={{color: "#888"}}>Checking&hellip;</span>
                        : (running
                            ? <span style={{color: "#18a300"}}>Running</span>
                            : <span style={{color: "#a70000"}}>Stopped</span>)}
                </div>
                {busy ? <div style={{color: "#777", fontSize: "0.85em"}}>Working&hellip;</div> : null}
            </div>
        </div>;
    }
}
