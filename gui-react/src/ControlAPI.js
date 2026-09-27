import {errorWindow, informationWindow} from "./components/MessageWindow";

export default class ControlAPI {

    static token = "";

    /**
     * Where the control API lives.
     *
     * The interface is served by the very server it configures, so the page's own origin is the
     * right target - and the only one that is right when the page is opened on another host. A
     * fixed "localhost" sends every call to whichever server runs on the machine holding the
     * browser, so opening http://other-host:18883 shows, and edits, the local server instead.
     *
     * The exception is the development server (npm start, port 3000), which serves the page but
     * not the API; there the API is taken to be on the same host at the default service port.
     */
    static serviceURL = ControlAPI.defaultHost() + ":" + ControlAPI.defaultPort();

    /**
     * Scheme and host the interface was served from, e.g. "https://broker.example".
     *
     * The development server (npm start, port 3000) serves the page over plain HTTP but is not
     * the API, so its scheme says nothing about the server's: the API is reached over HTTPS,
     * which is what the server serves unless it has been configured otherwise.
     *
     * @return {string} scheme and host part of the default service URL.
     */
    static defaultHost() {
        const scheme = window.location.port === "3000" ? "https:" : window.location.protocol;
        return scheme + "//" + (window.location.hostname || "localhost");
    }

    /**
     * Port the control API is expected on: the one this page came from. The development server
     * (npm start, port 3000) serves the page but not the API, so there the default port is used.
     * @return {number|string} port of the default service URL.
     */
    static defaultPort() {
        const port = window.location.port;
        return (!port || port === "3000") ? 18883 : port;
    }
    /**
     * The address this interface would have on another port, or under another scheme.
     *
     * Built from the address the page was opened at, not from a host name the server reports: the
     * browser reached this machine somehow - a name, an address, a tunnel - and that is the route
     * that will still work on the new port.
     *
     * @param {number|string} port  Port the interface has moved to.
     * @param {?string} scheme      "https:" or "http:", or omitted to keep the current one.
     * @return {string} address to send the browser to.
     */
    static interfaceUrl(port, scheme = null) {
        return (scheme || window.location.protocol) + "//" +
               window.location.hostname + ":" + port + "/";
    }

    /**
     * The interface's address, spelled so the browser has to open a new connection to reach it.
     *
     * Setup issues the interface a new certificate. A browser goes on using the connections it
     * already has, which were established under the old one, so the page that follows the setup
     * loads without the new certificate ever being shown - and then the extra connections that
     * page opens do present it, are refused, and fail with no response at all. What that looks
     * like is a Dashboard sitting at "Checking..." while the server is perfectly well.
     *
     * Connections are pooled per host *string*, so the two spellings of the loopback address are
     * two separate pools: going to the other one is certain to need a new connection, which
     * presents the new certificate where a browser will ask about it. Both spellings are on the
     * certificate, so nothing is mismatched.
     *
     * Any other host is left as it is. It means the interface was reached from another machine,
     * which is not the first-run path, and its name is not ours to rewrite.
     *
     * @param {number|string} port  Port the interface is on.
     * @return {string} address to send the browser to.
     */
    static freshInterfaceUrl(port) {
        const otherSpelling = {"127.0.0.1": "localhost", "localhost": "127.0.0.1"};
        const host = otherSpelling[window.location.hostname] || window.location.hostname;
        return window.location.protocol + "//" + host + ":" + port + "/";
    }

    /**
     * Everything that needs to react to signing in or out.
     *
     * A list rather than a single callback: the menu and the route guard both listen, and with one
     * slot the second registration would quietly displace the first - leaving a menu that never
     * updates, or pages that stay open after the session ends.
     */
    static connectedListeners = [];

    /**
     * Registers a listener for connection-state changes.
     * @param {function(boolean)} onConnectedChanged  Called with the new connected state.
     * @return {function()} unsubscribes the listener; call it when the component unmounts.
     */
    static setOnConnectedChange(onConnectedChanged) {
        ControlAPI.connectedListeners.push(onConnectedChanged);
        return () => {
            ControlAPI.connectedListeners =
                ControlAPI.connectedListeners.filter(listener => listener !== onConnectedChanged);
        };
    }

    static onConnectedChanged(isConnected) {
        ControlAPI.connectedListeners.forEach(listener => listener(isConnected));
    }

    static connected() {
        return ControlAPI.token !== "";
    }

    /**
     * Whether the server has still to be set up, as the last sign-in reported.
     *
     * True on a server whose administrator account has no password yet - a fresh installation.
     * The server admits the administrator without one while that lasts, and answers on the
     * loopback address alone, so setting a password is the one thing worth doing next.
     */
    static setupRequired = false;

    /**
     * Version of the server this interface is talking to.
     *
     * Empty until the server has said so. The page header used to carry a version written into
     * the interface itself, which is a different thing and was wrong the moment either side moved:
     * the interface can be newer or older than the server it is pointed at, and only the server
     * knows what the server is.
     */
    static serverVersion = "";

    static versionListeners = [];

    /**
     * Registers a listener for the server version becoming known.
     * @param {function(string)} onVersionChanged Called with the version.
     * @return {function()} unsubscribes the listener.
     */
    static setOnServerVersionChange(onVersionChanged) {
        ControlAPI.versionListeners.push(onVersionChanged);
        return () => {
            ControlAPI.versionListeners =
                ControlAPI.versionListeners.filter(listener => listener !== onVersionChanged);
        };
    }

    static setServerVersion(version) {
        if (!version || version === ControlAPI.serverVersion) {
            return;
        }
        ControlAPI.serverVersion = version;
        ControlAPI.versionListeners.forEach(listener => listener(version));
    }

    /**
     * Recognises a reply that means the session is no longer valid.
     *
     * The token expires on the server, and nothing tells the page when that moment arrives - it
     * finds out by being refused. Until it does, the interface looks signed in while every call
     * fails.
     * @param {?Object} result  Reply from asyncMakeAPICall.
     * @return {boolean} true if the reply says the session is gone.
     */
    static isSessionExpired(result) {
        if (!result) {
            return false;
        }
        const reason = result.error_description ||
                       (result.result && !result.result.success ? result.result.description : null);
        if (!reason) {
            return false;
        }
        return /not authenticated|session expired/i.test(reason);
    }

    /**
     * Performs an API call to the specified `apiMethod` with the given `content` object.
     * @param {string} apiMethod - The name of the API call to make
     * @param {Object} content - The JSON object to pass as the body of the request
     * @return {Promise<Object|Error>} A promise that resolves to the JSON response
     *  from the API, or rejects with an error if the call fails
     */
    static async asyncMakeAPICall(apiMethod, content, quiet = false) {
        try {
            let headers = {'Content-Type': 'application/json'};
            if (ControlAPI.connected()) {
                headers['Authorization'] = 'Bearer ' + ControlAPI.token;
            }
            // No catch on the fetch itself. The one that used to be here read err.response.data,
            // and a network failure has no "response" - so reading it threw a TypeError of its
            // own, which is what arrived below instead of the real reason. The failure is handled
            // in one place, where the reason is still intact.
            const response = await fetch(this.serviceURL + '/' + apiMethod,
                {
                    body: JSON.stringify(content),
                    headers: headers,
                    method: 'POST',
                    mode: 'cors'
                });
            const json = await response.json();
            // Noticed here rather than in each screen: any refused call is proof the session has
            // ended, and signing out from one place keeps the menu and the open page in step.
            if (ControlAPI.connected() && ControlAPI.isSessionExpired(json)) {
                ControlAPI.logout();
            }
            return json;
        } catch (error) {
            console.log(apiMethod + " failed: " + error);
            if (!quiet) {
                // What is actually known: this call did not get through. Whether the broker is
                // running is a different question, answered on the Dashboard and not here - and
                // saying it is offline when the request simply did not arrive sends people
                // looking for a fault in the server that is not there.
                errorWindow("The XMQ interface did not answer the request.");
            }
            return null;
        }
    }

    /**
     * Describes what went wrong with an API reply, or returns null when it succeeded.
     *
     * Replies come back in two shapes. A request the service handled and refused carries
     * result.success = false with a description. A request rejected before that - by the schema,
     * or by authentication - carries error_code and error_description and no result at all, so
     * reading result.result.success on one of those throws instead of reporting the reason.
     *
     * @param {Object} result   Reply from asyncMakeAPICall.
     * @return {?string} the reason, or null if the call succeeded.
     */
    static apiError(result) {
        if (result.error_description) {
            return result.error_description;
        }
        if (!result.result) {
            return "Malformed reply from the server";
        }
        return result.result.success ? null : (result.result.description || "Unknown error");
    }

    /**
     * Signs in.
     * @return {Promise<boolean>} whether the sign-in succeeded, so the caller can move on.
     */
    static login(username, password) {
        return ControlAPI.asyncMakeAPICall("Login", {"username": username, "password": password})
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return false;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Login failed: " + error);
                    return false;
                }
                ControlAPI.token = result.token;
                ControlAPI.setupRequired = result.setup_required === true;
                ControlAPI.onConnectedChanged(true);
                return true;
            });
    }

    static logout() {
        ControlAPI.token = "";
        ControlAPI.onConnectedChanged(false);
    }

    static setPersistence(persistence) {
        ControlAPI.asyncMakeAPICall("PersistenceControl", {action: "set", persistence: persistence})
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return false;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't change the persistence settings: " + error);
                    return false;
                }
                informationWindow("Persistence parameters changed, please restart XMQ to take effect");
                return true;
            });
    }

    static installKeys(keys) {
        ControlAPI.asyncMakeAPICall("SSLKeysControl", {action: "set", keys: keys})
            .then(result => {
                if (result === null) {
                    // XMQ server is offline
                    return false;
                }
                const error = ControlAPI.apiError(result);
                if (error) {
                    errorWindow("Can't install the keys: " + error);
                    return false;
                }
                return true;
            });
    }
}
