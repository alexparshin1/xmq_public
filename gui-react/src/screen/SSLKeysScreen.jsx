import React from 'react';
import "./InputScreen.css"
import FileLoad from "../components/FileLoad";
import ControlAPI from "../ControlAPI";
import HelpButton from "../components/HelpButton";
import PageTitle from "../components/PageTitle";
import {confirmationWindow, errorWindow, informationWindow} from "../components/MessageWindow";

/**
 * SSL keys screen.
 * The fields match the SSLKeys and WebServiceKeys types in xmq.wsdl, that is "connections.ssl_keys"
 * and the certificate named by "web_service".
 *
 * The configuration holds the paths of the installed files, while this screen uploads their
 * content: a chosen file is read in the browser and sent, and the server writes it into its
 * certificates directory and records the path. A key left untouched keeps what is installed.
 *
 * Two certificates live here, and they are not the same one. The broker proves its identity to
 * MQTT clients; the configuration interface proves its identity to browsers. They are usually
 * reached under different names, so each is installed on its own.
 */
export default class SSLKeysScreen extends React.Component {
    state = {
        // Paths of the installed broker files, as reported by the server.
        installed: {
            cafile: "",
            keyfile: "",
            certfile: ""
        },
        // Content of the files chosen in this session, keyed the same way. Only these are sent.
        uploaded: {},
        // Names of the chosen files, for showing what is about to be installed.
        chosenNames: {},
        verify_depth: 0,

        // The same three things for the interface's own certificate, plus what that certificate
        // says about itself - subject, expiry and fingerprint, as the server reads them.
        webface: {
            certfile: "",
            keyfile: "",
            description: ""
        },
        webfaceUploaded: {},
        webfaceChosenNames: {},

        // The nodes this one trusts on bridge and cluster links, and this node's own certificate
        // in PEM form - what the other nodes have to be given.
        peers: [],
        nodeCertificate: "",
        peerName: "",
        peerCertificate: "",
        peerFileName: "",

        // A note from the last save that is worth leaving on screen rather than in a popup.
        message: ""
    };

    componentDidMount() {
        ControlAPI.asyncMakeAPICall("SSLKeysControl", {action: "get"})
            .then(result => this.applyResult(result));
    }

    /**
     * Takes what the server reported into the form. Used after loading and after every save, so
     * that the screen shows what is installed rather than what was typed.
     */
    applyResult(result) {
        if (!result) {
            return;
        }

        const keys = result.keys;
        if (keys) {
            this.setState({
                installed: {
                    cafile: keys.cafile ? keys.cafile : "",
                    keyfile: keys.keyfile ? keys.keyfile : "",
                    certfile: keys.certfile ? keys.certfile : ""
                },
                verify_depth: keys.verify_depth ? keys.verify_depth : 0
            });
        }

        this.setState({
            peers: result.peers ? result.peers : [],
            nodeCertificate: result.node_certificate ? result.node_certificate : ""
        });

        const webface = result.web_service_keys;
        if (webface) {
            this.setState({
                webface: {
                    certfile: webface.certfile ? webface.certfile : "",
                    keyfile: webface.keyfile ? webface.keyfile : "",
                    description: webface.description ? webface.description : ""
                }
            });
        }
    }

    installKeys() {
        const uploaded = this.state.uploaded;
        if (Object.keys(uploaded).length === 0) {
            errorWindow("Choose at least one certificate or key file to install.");
            return;
        }

        ControlAPI.asyncMakeAPICall("SSLKeysControl", {
            action: "set",
            keys: {
                // An empty field means "keep the installed file".
                cafile: uploaded.cafile ? uploaded.cafile : "",
                keyfile: uploaded.keyfile ? uploaded.keyfile : "",
                certfile: uploaded.certfile ? uploaded.certfile : "",
                verify_depth: String(this.state.verify_depth)
            }
        }).then(result => {
            if (result === null) {
                // XMQ server is offline
                return;
            }
            const error = ControlAPI.apiError(result);
            if (error) {
                errorWindow("Can't install the keys: " + error);
                return;
            }
            informationWindow("The keys are installed. Existing MQTT+SSL connections keep the previous "
                  + "certificate until they reconnect.");
            this.setState({uploaded: {}, chosenNames: {}});
            this.applyResult(result);
        });
    }

    installWebfaceKeys() {
        const uploaded = this.state.webfaceUploaded;

        // Both together: a certificate installed over a key that does not match it would leave the
        // interface unable to complete a handshake, and this page is what would have to fix it.
        if (!uploaded.certfile || !uploaded.keyfile) {
            errorWindow("Choose both the certificate and its private key.");
            return;
        }

        ControlAPI.asyncMakeAPICall("SSLKeysControl", {
            action: "set",
            web_service_keys: {certfile: uploaded.certfile, keyfile: uploaded.keyfile}
        }).then(result => this.afterWebfaceChange(result, "Can't install the certificate: "));
    }

    async reissueWebfaceCertificate() {
        if (!await confirmationWindow("Issue a new self-signed certificate for this interface?\n\n"
                            + "The certificate installed now is replaced. Browsers that accepted "
                            + "the old one will warn again, because the new one has a different "
                            + "fingerprint.")) {
            return;
        }

        ControlAPI.asyncMakeAPICall("SSLKeysControl", {action: "reissue"})
            .then(result => this.afterWebfaceChange(result, "Can't issue a certificate: "));
    }

    /**
     * Shared ending for installing and reissuing: report what happened and show what is installed.
     */
    afterWebfaceChange(result, errorPrefix) {
        if (result === null) {
            // XMQ server is offline
            return;
        }

        const error = ControlAPI.apiError(result);
        if (error) {
            this.setState({message: errorPrefix + error});
            return;
        }

        // A description on a successful reply says what happened to the running interface: the
        // files are installed either way, and what is served may not have changed yet.
        const note = result.result && result.result.description ? result.result.description : "";
        this.setState({
            webfaceUploaded: {},
            webfaceChosenNames: {},
            message: note ? note : "The certificate is installed."
        });
        this.applyResult(result);
    }

    trustPeer() {
        if (!this.state.peerName || !this.state.peerCertificate) {
            errorWindow("Give the node a name and choose its certificate file.");
            return;
        }

        ControlAPI.asyncMakeAPICall("SSLKeysControl", {
            action: "trust-peer",
            peer: {name: this.state.peerName, certificate: this.state.peerCertificate}
        }).then(result => {
            if (result === null) {
                return;
            }
            const error = ControlAPI.apiError(result);
            if (error) {
                this.setState({message: "Can't trust that node: " + error});
                return;
            }
            this.setState({
                peerName: "", peerCertificate: "", peerFileName: "",
                message: "Links to that node are verified against its certificate from now on."
            });
            this.applyResult(result);
        });
    }

    async distrustPeer(name) {
        if (!await confirmationWindow("Stop trusting " + name + "?\n\n"
                            + "Bridge and cluster links to it stop being verifiable, and will "
                            + "fail if they are set to verify.")) {
            return;
        }

        ControlAPI.asyncMakeAPICall("SSLKeysControl", {
            action: "distrust-peer",
            peer: {name: name}
        }).then(result => {
            if (result === null) {
                return;
            }
            const error = ControlAPI.apiError(result);
            if (error) {
                this.setState({message: "Can't stop trusting that node: " + error});
                return;
            }
            this.setState({message: name + " is no longer trusted."});
            this.applyResult(result);
        });
    }

    onFileChosen(fieldName, fileName, fileContent) {
        this.setState({
            uploaded: {...this.state.uploaded, [fieldName]: fileContent},
            chosenNames: {...this.state.chosenNames, [fieldName]: fileName}
        });
    }

    onWebfaceFileChosen(fieldName, fileName, fileContent) {
        this.setState({
            webfaceUploaded: {...this.state.webfaceUploaded, [fieldName]: fileContent},
            webfaceChosenNames: {...this.state.webfaceChosenNames, [fieldName]: fileName}
        });
    }

    /**
     * Describes what will be installed for a key: the file just chosen, or what is installed
     * already, or nothing at all.
     */
    renderKeyState(fieldName, chosenNames, installed) {
        if (chosenNames[fieldName]) {
            return <span className="inputScreenNote">
                to install: {chosenNames[fieldName]}
            </span>;
        }
        if (installed[fieldName]) {
            return <span className="inputScreenNote">
                installed: {installed[fieldName]}
            </span>;
        }
        return <span className="inputScreenNote">not installed</span>;
    }

    renderSslKey(label, fieldName, helpKey = null) {
        return <div className="inputScreenRow">
            <label htmlFor={fieldName} className="inputScreenLabel">{label}:</label>
            <FileLoad name={fieldName} accept=".crt,.key,.pem,.cert"
                      onLoad={(fileName, fileContent) =>
                          this.onFileChosen(fieldName, fileName, fileContent)}/>
            <HelpButton helpKey={helpKey}/>
            {this.renderKeyState(fieldName, this.state.chosenNames, this.state.installed)}
        </div>
    }

    renderWebfaceKey(label, fieldName, helpKey = null) {
        return <div className="inputScreenRow">
            <label htmlFor={"webface_" + fieldName} className="inputScreenLabel">{label}:</label>
            <FileLoad name={"webface_" + fieldName} accept=".crt,.key,.pem,.cert"
                      onLoad={(fileName, fileContent) =>
                          this.onWebfaceFileChosen(fieldName, fileName, fileContent)}/>
            <HelpButton helpKey={helpKey}/>
            {this.renderKeyState(fieldName, this.state.webfaceChosenNames, this.state.webface)}
        </div>
    }

    render() {
        return <div>
            <PageTitle>SSL Keys</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Certificates used by the MQTT+SSL listeners. Choosing a file uploads its
                    content to the server, which stores it in its own certificates directory:
                    /etc/xmq/certs on Linux, C:/ProgramData/xmq/certs on Windows.
                </p>
                <p>
                    A key left untouched keeps the file already installed. The previous file is
                    kept alongside the new one with an .old suffix.
                </p>
            </div>

            {this.renderSslKey("Server CA certificate", "cafile")}
            {this.renderSslKey("Server key", "keyfile")}
            {this.renderSslKey("Server certificate", "certfile")}

            <div className="inputScreenRow">
                <label htmlFor="verify_depth" className="inputScreenLabel">Verify depth:</label>
                <input name="verify_depth" type="number" style={{width: '70px'}}
                       min={0} max={10}
                       value={this.state.verify_depth}
                       onChange={(e) => this.setState({verify_depth: e.target.value})}/>
                <HelpButton helpKey="ssl_keys.verify_depth"/>
            </div>

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" onClick={e => this.installKeys()}>
                    Install Keys
                </button>
            </div>

            <PageTitle>Configuration Interface Certificate</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    The certificate this page itself is served with. It is not the broker's: the
                    two are reached under different names as often as not, so replacing one here
                    leaves the other alone.
                </p>
                <p>
                    Unless a certificate has been installed, the server issues one to itself, and
                    browsers warn about it on the first visit &mdash; the fingerprint below is what
                    that warning should be checked against. Installing a certificate from a
                    certificate authority the browsers already know removes the warning.
                </p>
                <p>
                    A new certificate is served to connections made from then on, without a
                    restart. This page keeps the one it opened with until it is reloaded, so
                    replacing a certificate never locks anyone out of the interface.
                </p>
            </div>

            {this.state.webface.description
                ? <div className="inputScreenRow">
                    <label className="inputScreenLabel">Serving:</label>
                    <span className="inputScreenNote" style={{wordBreak: "break-all"}}>
                        {this.state.webface.description}
                    </span>
                </div>
                : null}

            {this.renderWebfaceKey("Interface certificate", "certfile", "web_service.certfile")}
            {this.renderWebfaceKey("Interface key", "keyfile", "web_service.keyfile")}

            <div className="inputScreenRow">
                <label className="inputScreenLabel"/>
                <button className="inputScreenSaveButton" onClick={e => this.installWebfaceKeys()}>
                    Install Certificate
                </button>
                <button className="inputScreenSaveButton" style={{marginLeft: "10px"}}
                        onClick={e => this.reissueWebfaceCertificate()}>
                    Issue Self-Signed
                </button>
            </div>

            <PageTitle>Trusted Nodes</PageTitle>

            <div className="inputScreenIntro">
                <p>
                    Certificates of the other XMQ servers this one bridges or clusters to. Links
                    between brokers are between servers one administrator runs, so there is no
                    certificate authority in the picture and none is needed: each node holds the
                    certificates of the nodes it talks to, and a self-signed certificate vouches
                    for itself.
                </p>
                <p>
                    Without this a link can be encrypted and still be answered by whatever takes
                    the address. A bridge set to verify, with nothing here that vouches for the
                    far node, does not connect at all &mdash; which is the point.
                </p>
            </div>

            <div className="inputScreenRow">
                <label className="inputScreenLabel">This node&apos;s certificate:</label>
                <textarea readOnly rows={4} style={{width: "460px", fontFamily: "monospace",
                                                    fontSize: "11px"}}
                          value={this.state.nodeCertificate}
                          onFocus={e => e.target.select()}/>
                <span className="inputScreenNote">give this to the other nodes</span>
            </div>

            {this.state.peers.length === 0
                ? <div className="inputScreenRow">
                    <label className="inputScreenLabel"/>
                    <span className="inputScreenNote">No node is trusted yet.</span>
                </div>
                : this.state.peers.map(peer =>
                    <div className="inputScreenRow" key={peer.name}>
                        <label className="inputScreenLabel">{peer.name}:</label>
                        <span className="inputScreenNote" style={{wordBreak: "break-all",
                                                                  maxWidth: "520px"}}>
                            {peer.description}
                        </span>
                        <button className="inputScreenSaveButton" style={{marginLeft: "10px"}}
                                onClick={e => this.distrustPeer(peer.name)}>
                            Remove
                        </button>
                    </div>)}

            <div className="inputScreenRow">
                <label htmlFor="peer_name" className="inputScreenLabel">Trust node:</label>
                <input name="peer_name" type="text" style={{width: "160px"}}
                       placeholder="name"
                       value={this.state.peerName}
                       onChange={e => this.setState({peerName: e.target.value})}/>
                <FileLoad name="peer_certificate" accept=".crt,.pem,.cert"
                          onLoad={(fileName, fileContent) =>
                              this.setState({peerCertificate: fileContent, peerFileName: fileName})}/>
                <button className="inputScreenSaveButton" style={{marginLeft: "10px"}}
                        onClick={e => this.trustPeer()}>
                    Trust
                </button>
                {this.state.peerFileName
                    ? <span className="inputScreenNote">{this.state.peerFileName}</span>
                    : null}
            </div>

            {this.state.message
                ? <div className="inputScreenRow">
                    <label className="inputScreenLabel"/>
                    <span className="inputScreenNote" style={{whiteSpace: "pre-wrap"}}>
                        {this.state.message}
                    </span>
                </div>
                : null}
        </div>
    }
}
