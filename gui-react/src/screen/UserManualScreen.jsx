import React from "react";
import "./UserManual.css";

/**
 * XMQ user manual, served as a single page from the configuration interface.
 * The content describes the installed server, so it is kept next to the interface
 * rather than published separately: what it documents is what is running.
 *
 * The same page is published on the SPTK website. This file and UserManual.css are copied
 * verbatim into the site's site-react/src/pages/, so this copy is the original and the website
 * one is downstream of it. Two things keep a plain copy sufficient: the page depends on nothing
 * from either application - no ControlAPI, no shared components - and its wording names the
 * configuration interface rather than saying "this interface", which would be wrong on the
 * website, where the reader is not in it. Keep both when editing.
 */
export default class UserManualScreen extends React.Component {

    renderContents() {
        const sections = [
            ["supported-oses", "Supported operating systems"],
            ["install", "Download and install binaries"],
            ["tuning", "Environment tuning"],
            ["basic-setup", "Basic MQTT setup"],
            ["advanced-setup", "Advanced setup"],
            ["testing-suite", "MQTT testing suite"],
            ["testing-setup", "Testing your setup"]
        ];

        return <div className="userManualContents">
            <ul>
                {sections.map(([anchor, title]) =>
                    <li key={anchor}><a href={"#" + anchor}>{title}</a></li>)}
            </ul>
        </div>;
    }

    renderSupportedOses() {
        return <div>
            <h4 id="supported-oses">Supported operating systems</h4>

            <p>
                XMQ is built and packaged for the systems below. Each is a 64-bit x86 build,
                packaged in the format native to that system.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>System</th>
                    <th>Versions</th>
                    <th>Package</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>Debian</td>
                    <td>trixie, forky</td>
                    <td><code>.deb</code></td>
                </tr>
                <tr>
                    <td>Ubuntu</td>
                    <td>25.04, 25.10, 26.04</td>
                    <td><code>.deb</code></td>
                </tr>
                <tr>
                    <td>Fedora</td>
                    <td>42, 43, 44</td>
                    <td><code>.rpm</code></td>
                </tr>
                <tr>
                    <td>Oracle Linux</td>
                    <td>10</td>
                    <td><code>.rpm</code></td>
                </tr>
                <tr>
                    <td>FreeBSD</td>
                    <td>15</td>
                    <td><code>pkg</code></td>
                </tr>
                <tr>
                    <td>Windows</td>
                    <td>10, 11, Server</td>
                    <td>installer</td>
                </tr>
                </tbody>
            </table>

            <p>
                Other Linux distributions of comparable vintage generally work when built from
                source, but only the versions listed here are tested and packaged.
            </p>
        </div>;
    }

    renderInstall() {
        return <div>
            <h4 id="install">Download and install binaries</h4>

            <p>
                The only official download location is <a href="https://sptk.net/Downloads">here</a>.
                Select the latest SPTK version and your operating system, and download xmq_server package.
            </p>

            <p>
                Packages are named <code>XMQ-server</code> and install under <code>/usr/local</code>
                on Linux and FreeBSD. The server, the utilities, the configuration interface, and the load-test scenarios all
                come from that one package.
            </p>

            <p>
                The dependencies for Linux include brotli and pcre2 packages that are included in all major
                Linux distributions. On FreeBSD <code>pkg</code> resolves them from the official
                repository. The dependencies for Windows are installed with XMQ.
            </p>

            <p>
                The package file name carries the version and the architecture, so the commands
                below use a wildcard. Run them in the directory the package was downloaded to.
            </p>

            <h5>Debian and Ubuntu</h5>
            <pre className="userManualCode">{`sudo apt install ./xmq-server_*.deb`}</pre>

            <h5>Fedora and Oracle Linux</h5>
            <pre className="userManualCode">{`sudo dnf install ./XMQ-server-*.rpm`}</pre>

            <h5>FreeBSD</h5>
            <pre className="userManualCode">{`sudo pkg install ./XMQ-server-*.pkg`}</pre>

            <p>
                Installing through the package manager rather than <code>dpkg -i</code> or
                <code>rpm -i</code> lets it resolve the shared libraries the package depends on.
            </p>

            <h5>What gets installed</h5>
            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Path</th>
                    <th>Contents</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>/usr/local/bin</code></td>
                    <td><code>xmq_server</code> and the <code>xmq_pub</code>, <code>xmq_sub</code>,
                        <code>xmq_con</code>, <code>xmq_scn</code> utilities
                    </td>
                </tr>
                <tr>
                    <td><code>/etc/xmq/xmq_server.conf</code></td>
                    <td>Server configuration. An existing file is never overwritten by an upgrade</td>
                </tr>
                <tr>
                    <td><code>/etc/xmq/xmq_server.conf.template</code></td>
                    <td>The shipped defaults, refreshed on every upgrade, for comparison</td>
                </tr>
                <tr>
                    <td><code>/etc/xmq/certs</code></td>
                    <td>TLS certificates and keys installed from the SSL Keys page</td>
                </tr>
                <tr>
                    <td><code>/var/log/xmq</code></td>
                    <td>Server log</td>
                </tr>
                <tr>
                    <td><code>/usr/local/share/xmq</code></td>
                    <td>Load-test scenarios, host tuning files, helper scripts</td>
                </tr>
                </tbody>
            </table>

            <p>
                On Windows the equivalents live under <code>C:\ProgramData\xmq</code>, with the
                program files in <code>C:\Program Files\xmq</code>.
            </p>

            <h5>Running the server</h5>
            <p>
                The package installs a systemd unit, so on Linux the server is managed the usual way:
            </p>
            <pre className="userManualCode">{`sudo systemctl enable --now xmq_server
sudo systemctl status xmq_server
sudo journalctl -u xmq_server -f`}</pre>

            <p>
                On FreeBSD the package installs an <code>rc.d</code> script instead:
            </p>
            <pre className="userManualCode">{`sudo sysrc xmq_server_enable=YES
sudo service xmq_server start
sudo service xmq_server status`}</pre>

            <p>
                On Windows, <code>xmq_server --install-service</code> registers it as a Windows
                service, and <code>--uninstall-service</code> removes it.
            </p>

            <p>
                Useful options when running it by hand:
            </p>
            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Option</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>-c</code>, <code>--configuration-file</code></td>
                    <td>Use a configuration file other than the default</td>
                </tr>
                <tr>
                    <td><code>-l</code>, <code>--log-file</code></td>
                    <td>Write the log somewhere other than the configured path</td>
                </tr>
                <tr>
                    <td><code>--use-cpu-affinity</code></td>
                    <td>Pin threads to one hardware thread per physical core. Affinity is opt-in and
                        Linux-only; it understands CPU sets, so it cooperates with containers
                    </td>
                </tr>
                <tr>
                    <td><code>-d</code>, <code>--debug</code></td>
                    <td>Verbose logging, regardless of the configured levels</td>
                </tr>
                <tr>
                    <td><code>--set-password</code></td>
                    <td>Set an account's password and exit, reading the password from standard
                        input. The way to give the administrator a password where the
                        configuration interface cannot be reached &mdash; see below
                    </td>
                </tr>
                <tr>
                    <td><code>--version</code></td>
                    <td>Print the version and exit</td>
                </tr>
                </tbody>
            </table>

            <h5>Setting the administrator password without the interface</h5>

            <p>
                A freshly installed server has an administrator with no password, and until one is
                set the configuration interface answers on <code>127.0.0.1</code> only. On a machine
                with a console that is exactly right. Where there is no console it is a problem, so
                the password can also be set from the command line:
            </p>

            <pre className="userManualCode">{`printf '%s' 'the-new-password' | sudo -u xmq xmq_server --set-password admin`}</pre>

            <p>
                The password is read from standard input, not taken as an argument, so it does not
                appear in the process list or the shell history. On a headless server the
                alternative is to forward the port &mdash;
                <code>ssh -L 18883:127.0.0.1:18883 the-server</code> &mdash; and use the interface
                as usual. In a container neither the console nor the loopback address is available,
                and the image sets the password from <code>XMQ_ADMIN_PASSWORD</code> for this
                reason.
            </p>

            <p>
                Once the server runs, the configuration interface is reachable on port 18883 by
                default. On a server that has just been installed it answers on that machine only,
                at <code>https://127.0.0.1:18883</code>, and the <code>admin</code> account it
                created has no password: sign in as <code>admin</code> leaving the password empty.
                The <b>Initial Setup</b> page opens straight away, and the password set there is
                what opens the interface to the rest of the network.
            </p>

            <div className="userManualNote">
                Nothing is shipped with a password. Until the administrator has one, the account
                cannot be used over MQTT at all and the interface cannot be reached from another
                machine &mdash; so the first sign-in has to be made from the server&apos;s own
                console, or through a forwarded port.
            </div>
        </div>;
    }

    renderTuning() {
        return <div>
            <h4 id="tuning">Environment tuning</h4>

            <p>
                A default Linux install is tuned for a few thousand sockets, not a few hundred
                thousand. Below roughly 10,000 concurrent connections the stock settings are fine
                and this section can be skipped. Above that, the limits below are the ones that
                bite, and each of them has been hit in real test runs.
            </p>

            <p>
                Ready-made files are installed with the package under
                <code>/usr/local/share/xmq/setup</code>, along with a README describing the symptom
                each one addresses:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>File</th>
                    <th>Install to</th>
                    <th>Purpose</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>sysctl.d/mqtt.conf</code></td>
                    <td><code>/etc/sysctl.d/</code></td>
                    <td>File descriptor ceiling, socket buffers, accept queue depth, TCP memory</td>
                </tr>
                <tr>
                    <td><code>sysctl.d/port_range.conf</code></td>
                    <td><code>/etc/sysctl.d/</code></td>
                    <td>Widen the ephemeral port range to 10000-65535</td>
                </tr>
                <tr>
                    <td><code>limits.d/mqtt.conf</code></td>
                    <td><code>/etc/security/limits.d/</code></td>
                    <td><code>nofile</code> for login sessions</td>
                </tr>
                <tr>
                    <td><code>modules-load.d/nf_conntrack.conf</code></td>
                    <td><code>/etc/modules-load.d/</code></td>
                    <td>Load <code>nf_conntrack</code> before <code>systemd-sysctl</code> runs</td>
                </tr>
                <tr>
                    <td><code>nftables.d/xmq-notrack.nft</code></td>
                    <td><code>/etc/nftables.d/</code></td>
                    <td>Exempt the MQTT ports from connection tracking</td>
                </tr>
                </tbody>
            </table>

            <pre className="userManualCode">{`cd /usr/local/share/xmq/setup
sudo install -m 0644 sysctl.d/mqtt.conf       /etc/sysctl.d/mqtt.conf
sudo install -m 0644 sysctl.d/port_range.conf /etc/sysctl.d/port_range.conf
sudo install -m 0644 limits.d/mqtt.conf       /etc/security/limits.d/mqtt.conf
sudo modprobe nf_conntrack
sudo sysctl --system`}</pre>

            <p>
                Then log out and back in, so the <code>nofile</code> limit applies to your shell.
            </p>

            <h5>The three that catch people out</h5>

            <ul>
                <li>
                    <b>One descriptor per connection, plus a handful.</b> The server idles at 17
                    descriptors, so a limit of exactly 1,000,000 fails at 999,983 connections. The
                    shipped files set 2,000,000, which leaves room.
                </li>
                <li>
                    <b>Services do not read <code>limits.d</code>.</b> That file applies to login
                    sessions only. A systemd service gets <code>DefaultLimitNOFILE</code> instead,
                    commonly 524288. The XMQ unit sets <code>LimitNOFILE</code> explicitly for this
                    reason; any unit you write yourself must do the same.
                </li>
                <li>
                    <b>A <code>ulimit -n</code> in your shell profile wins.</b> If
                    <code>~/.profile</code> or <code>~/.bashrc</code> lowers it, an unprivileged
                    process cannot raise it again, and the limit files appear to have no effect.
                </li>
            </ul>

            <p>
                Both ends need this. Every limit here is per host, and tuning only the server while
                leaving the client machine at defaults is the most common mistake. The
                <code>preflight.sh</code> script in <code>/usr/local/share/xmq</code> checks a host
                and exits non-zero if a required setting is missing:
            </p>

            <pre className="userManualCode">{`./preflight.sh                          # check this host as a client
./preflight.sh -H broker -r server -p 1883   # check a remote server host`}</pre>

            <h5 id="receive-steering">Receive packet steering on a single-queue network card</h5>

            <p>
                A network card with one receive queue delivers every incoming packet to one CPU, and
                the kernel runs the whole IP and TCP receive path there. Under heavy load that CPU
                reaches 100% while the others idle, the card starts dropping frames, and clients
                retransmit into backoff until the server closes their sessions for a keep-alive
                timeout - although every one of them was sending. On a test host with such a card, a
                100,000 messages a second run lost 255,891 frames a second this way and never
                finished. Linux receive packet steering (RPS) spreads the work over other CPUs, and
                with it the losses fell to zero and the run completed.
            </p>

            <p>
                XMQ checks this at start-up. When a network interface its listeners use has a single
                receive queue, no steering, and the machine has more than two CPUs, the log says so
                and links here. Enable RPS when all three of these hold:
            </p>

            <ul>
                <li>
                    <b>The card has one receive queue.</b> <code>ls /sys/class/net/eth0/queues/</code>
                    lists only <code>rx-0</code>. Cards with several queues already spread their
                    interrupts across CPUs and do not need RPS.
                </li>
                <li>
                    <b>One CPU is pinned under load.</b> While clients are connected and busy,
                    <code>mpstat -P ALL 1</code> shows <code>%soft</code> near 100 on one CPU.
                    Checking after the load has stopped shows nothing.
                </li>
                <li>
                    <b>The card is dropping frames.</b> The drop counters in
                    <code>ethtool -S eth0</code> - <code>rx_no_buffer_count</code>,
                    <code>rx_missed_errors</code> or <code>rx_fifo_errors</code>, depending on the
                    driver - grow while the load runs.
                </li>
            </ul>

            <p>
                With only the first condition, leave it off: moving packets between CPUs costs a
                little at low rates. To enable it, write a mask of the CPUs to use, and size the flow
                tables so that each packet goes to the CPU where its socket is read:
            </p>

            <pre className="userManualCode">{`IF=eth0
echo 70    | sudo tee /sys/class/net/$IF/queues/rx-0/rps_cpus      # hex mask: CPUs 4, 5 and 6
echo 32768 | sudo tee /proc/sys/net/core/rps_sock_flow_entries
echo 32768 | sudo tee /sys/class/net/$IF/queues/rx-0/rps_flow_cnt`}</pre>

            <p>
                Choose CPUs that are idle under load, not the one that receives the card's interrupt
                (<code>grep eth0 /proc/interrupts</code> shows which), and on the card's NUMA node. The
                settings do not survive a reboot. Keep them with a sysctl file and a udev rule:
            </p>

            <pre className="userManualCode">{`# /etc/sysctl.d/60-rfs.conf
net.core.rps_sock_flow_entries = 32768

# /etc/udev/rules/60-rps.rules
ACTION=="add", SUBSYSTEM=="net", KERNEL=="eth0", ATTR{queues/rx-0/rps_cpus}="70", ATTR{queues/rx-0/rps_flow_cnt}="32768"`}</pre>

            <p>
                Other systems have their own equivalents, which XMQ does not check. On FreeBSD it is
                netisr: <code>net.isr.maxthreads</code> and <code>net.isr.bindthreads</code> in
                <code>/boot/loader.conf</code>, with <code>net.isr.dispatch=deferred</code>. On Windows
                it is receive side scaling, set with <code>Set-NetAdapterRss</code>, and available only
                when the card's driver supports several queues.
            </p>
        </div>;
    }

    renderBasicSetup() {
        return <div>
            <h4 id="basic-setup">Basic MQTT setup</h4>

            <p>
                Everything in this section can be set from the configuration interface, and is written back to
                <code>/etc/xmq/xmq_server.conf</code>. The user data is stored in the file
                <code>/etc/xmq/xmq_users.conf</code>. Hand-editing these files is still possible;
                if you do, restart the server afterwards, since a running server rewrites the files
                when settings change and would overwrite your edit.
            </p>
            <p>
                The XMQ configuration interface is available through the browser at{" "}
                <code>https://xmq_host:18883</code>, where xmq_host is the host the XMQ server runs
                on. It is administered through the <code>admin</code> account. A server that has
                never been set up is the exception in two ways: it answers at{" "}
                <code>https://127.0.0.1:18883</code> and nowhere else, and <code>admin</code> has
                no password yet &mdash; sign in with the password field left empty, and set one on
                the <b>Initial Setup</b> page that opens.
            </p>

            <h5>Starting from scratch</h5>

            <p>
                The <b>Initial Setup</b> page asks for the few settings a working server cannot be
                without &mdash; an administrator password, a node name, the MQTT, MQTT+SSL, and
                interface ports, and optionally a Redis address &mdash; and builds a configuration
                around them. It is the quickest way to bring up a new installation, and the way
                back to a known state when a configuration has been edited into a corner.
            </p>

            <p>
                The <code>admin</code> account comes first on that page because it is what the
                interface is administered through: setup creates the account if it is not there,
                and gives it the password entered either way, so the configuration it produces
                always comes with a way in. Since a session carries the password it signed in with,
                changing it ends the current session &mdash; sign in again with the new password.
            </p>

            <p>
                On a server being set up for the first time, that password does one more thing: it
                is what moves the interface off the loopback address. Until it is set the interface
                accepts connections from the server&apos;s own machine only, because an account
                that anyone can sign in to without a password has no business being reachable from
                a network. The account is refused everywhere else in the meantime &mdash; an MQTT
                client offering <code>admin</code> and no password is not connected.
            </p>

            <p>
                It is the one page that discards settings rather than changing them. Everything it
                does not ask about is taken from <code>/etc/xmq/xmq_server.conf.template</code>, the
                copy of the shipped configuration the installer leaves behind, so listeners,
                bridges, limits, and logging settings all go back to their defaults. The template
                itself is only read; the result is written to <code>xmq_server.conf</code>.
            </p>

            <p>
                The accounts are reset with them: <code>xmq_users.conf</code> is rewritten with
                <code>admin</code>, using the password entered on the page, and the internal
                <code>cluster</code> account. MQTT clients that authenticate will need their
                accounts adding again on the Users page. Installed certificate files are kept,
                being files of their own rather than configuration.
            </p>

            <p>
                Submitting restarts the MQTT server, which disconnects every client. Changing the
                interface port takes effect at once too: the interface starts answering on the new
                port before it stops answering on the old one, and the browser is sent there.
                Connections already open are served to the end, so the reply announcing the move
                still arrives. A port that cannot be taken &mdash; because something else holds it
                &mdash; leaves the interface where it was, and the page says so instead of sending
                the browser nowhere.
            </p>

            <p>
                A missing configuration is not an error: the server writes a starting one from
                <code>xmq_server.conf.template</code>, or from its own defaults if the template is
                gone too, and comes up on the ports above. A missing <code>xmq_users.conf</code> is
                written too, with an <code>admin</code> account that has no password and the
                internal <code>cluster</code> account. No accounts are installed by the package:
                a password shipped in a file is a password everyone who downloaded the package
                knows.
            </p>

            <h5>When the interface will not start</h5>

            <p>
                The Initial Setup page needs the interface to be running, which is no help when the
                configuration is what stops the server from starting: a port already taken, a
                listener bound to an address this host does not have, or a file edited into
                something that will not parse. The same reset is available from the command line,
                where nothing needs to be running:
            </p>

            <pre className="userManualCode">{`sudo systemctl stop xmq
sudo xmq_server --reset-configuration   # or start the service again afterwards`}</pre>

            <p>
                It replaces <code>xmq_server.conf</code> with the starting configuration and carries
                on into a normal start. The file it replaced is kept beside it as
                <code>xmq_server.conf.old</code>, since it is usually still worth reading. Accounts
                are not touched &mdash; they are in their own file, and are not what stops a server
                from starting &mdash; except that one for <code>admin</code> is created, without a
                password, if there are no accounts at all.
            </p>

            <h5>Port numbers</h5>

            <p>
                Listeners are configured on the <b>Listeners</b> page. Each one is a port, a
                protocol, and a thread count. The shipped configuration defines two:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Port</th>
                    <th>Protocol</th>
                    <th>Purpose</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>1883</td>
                    <td>MQTT</td>
                    <td>Plain TCP, the standard MQTT port</td>
                </tr>
                <tr>
                    <td>8883</td>
                    <td>MQTT+SSL</td>
                    <td>TLS, the standard secure MQTT port</td>
                </tr>
                <tr>
                    <td>18883</td>
                    <td>HTTP</td>
                    <td>The configuration interface and its control API</td>
                </tr>
                </tbody>
            </table>

            <p>
                These are the standard MQTT ports, so most clients need no port setting at all.
                If another broker on the same host already holds them, either stop it or move one
                of the two: the Listeners page changes XMQ's ports, and the change takes effect as
                soon as the listener is saved.
            </p>

            <p>
                A listener also carries a bind address. <code>0.0.0.0</code> accepts on every
                interface; naming one address restricts the listener to it, which is a simple way
                to keep a port on an internal network. Disabling a listener keeps its settings but
                closes its port.
            </p>

            <p>
                The service port itself is set on the <b>Service</b> page, and changing it takes
                effect at once: the interface starts answering on the new port before it stops
                answering on the old one, and the page follows it there. Connections already open
                are served to the end. If something else already holds the port, the interface
                stays where it is and the page says so.
            </p>

            <h5>SSL certificates</h5>

            <p>
                An <code>MQTT+SSL</code> listener needs a server key and certificate. Install them
                from the <b>SSL Keys</b> page: choosing a file uploads its contents, and the server
                writes it into <code>/etc/xmq/certs</code>
                (<code>C:\ProgramData\xmq\certs</code> on Windows) and records the path. A previous
                file of the same name is kept beside the new one with an <code>.old</code> suffix.
            </p>

            <ul>
                <li><b>Server key</b> and <b>server certificate</b> are required for TLS.</li>
                <li><b>CA certificate</b> is needed only to verify client certificates.</li>
                <li>
                    <b>Verify depth</b> of 0 means clients are not asked for a certificate: the TLS
                    listener proves the server's identity and no more. Above 0, a client must
                    present a certificate that the CA certificate validates, within that many
                    intermediate signers.
                </li>
            </ul>

            <p>
                If the keys are missing or fail to load, the server logs an error, skips the
                MQTT+SSL listener, and carries on serving the plain one. That is worth checking in
                the log after installing certificates for the first time.
            </p>

            <h5>Logging</h5>

            <p>
                The <b>Logging</b> page sets the log file and the level per subject. The
                <b>minimum log level</b> is a ceiling rather than a floor: each subject has its own
                level, and the server clips it to this one. Turning the ceiling down to
                <code>INFO</code> quietens everything without touching the individual subjects,
                and turning it up to <code>DEBUG</code> lets them speak at their configured level.
            </p>

            <p>
                Levels are <code>PANIC</code>, <code>ERROR</code>, <code>WARNING</code>,
                <code>NOTICE</code>, <code>INFO</code>, <code>DEBUG</code>, from quietest to most
                verbose. Two subjects deserve care: <b>Publish</b> and <b>Acks</b> produce a line
                per message, not per client. At the rates XMQ is built for, <code>DEBUG</code>
                there can outweigh everything else the server logs and cost real throughput. Leave
                them at <code>ERROR</code> unless you are chasing something specific.
            </p>

            <p>
                Log rotation is left to the system. Session errors are always logged at
                <code>ERROR</code> and are not configurable.
            </p>

            <h5>Users and anonymous access</h5>

            <p>
                Accounts on the <b>Users</b> page serve both MQTT clients and the configuration interface. An
                administrator may change the configuration; a non-administrator can connect but not
                administer. <b>Allow anonymous</b> lets MQTT clients connect with no credentials at
                all; it does not affect the configuration interface, which always requires signing in.
            </p>

            <h5>Persistence</h5>

            <p>
                Without persistence the server keeps everything in memory and nothing survives a
                restart. With it, client sessions, their subscriptions, and undelivered QoS 1 and
                QoS 2 messages are stored in Redis. Redis is a separate service, not part of XMQ:
                install and start it yourself, then enable persistence on the <b>Persistence</b>
                page.
            </p>

            <p>
                If the server cannot reach Redis at startup it logs the error and keeps running
                with in-memory storage. The server stays up and nothing is persisted, so the log is
                worth a look after enabling it.
            </p>

            <p>
                One setting there is a genuine trade-off. <b>Max queued writes</b> at 0 makes every
                message wait for its own record to become durable before it is sent: the safest
                setting, and the slowest. Above 0, record writes pipeline instead, roughly doubling
                throughput, and the value is approximately how many messages could be lost if the
                server were killed outright.
            </p>
        </div>;
    }

    renderAdvancedSetup() {
        return <div>
            <h4 id="advanced-setup">Advanced setup</h4>

            <h5>MQTT bridges</h5>

            <p>
                A bridge is a client connection XMQ makes to another MQTT broker, carrying an
                agreed set of topics. The remote broker does not have to be XMQ: bridging to
                Mosquitto, EMQX, or anything else speaking MQTT works the same way. Bridges are
                configured on the <b>Bridges</b> page.
            </p>

            <p>
                A bridge has a direction, and so does each of its topics:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Mode</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>out</code></td>
                    <td>Messages published on this server are forwarded to the remote one</td>
                </tr>
                <tr>
                    <td><code>in</code></td>
                    <td>This server subscribes to the remote one and republishes what it sends</td>
                </tr>
                <tr>
                    <td><code>inout</code></td>
                    <td>Both directions</td>
                </tr>
                </tbody>
            </table>

            <p>
                Each topic entry carries a pattern, a direction of its own, and the QoS to
                subscribe with. A topic's direction narrows the bridge's: a topic marked
                <code>out</code> on an <code>inout</code> bridge travels outbound only. Topics keep
                the name they were published with; there is no rewriting between the two servers.
                A bridge with no topics carries nothing.
            </p>

            <h5>Things worth getting right</h5>

            <ul>
                <li>
                    <b>Client ID must be stable.</b> The remote broker keys the bridge's stored
                    subscriptions and queued messages by it. An id that changes between reconnects
                    leaves orphaned sessions accumulating on the remote. Left empty, XMQ derives one
                    from the two node names, which is stable.
                </li>
                <li>
                    <b>Leave clean session off.</b> The remote then keeps the subscriptions and
                    queues messages while the link is down, so a brief outage does not lose them.
                    With it on, the session starts empty each time and anything published while
                    disconnected is gone.
                </li>
                <li>
                    <b>Loops.</b> XMQ marks its bridge connections with an origin node, so it will
                    not echo back a message that arrived over the bridge. Bridging through a broker
                    that does not do the same, in both directions, on overlapping topics, can still
                    produce a loop.
                </li>
                <li>
                    <b>Encrypted bridges</b> use key files already present on the XMQ host, given by
                    path. That is the opposite of the SSL Keys page, which uploads content. The
                    remote broker must be listening on its TLS port.
                </li>
            </ul>

            <h5>Bridging two XMQ servers, step by step</h5>

            <p>
                This walks through joining two XMQ servers so that a client on either one sees
                traffic published on the other. Call them <b>mq-a</b> and <b>mq-b</b>; substitute
                your own host names throughout. One bridge is enough, defined on <b>mq-a</b> and
                pointing at <b>mq-b</b>, in <code>inout</code> mode - that single bridge carries
                both directions. Do not define a second one on mq-b pointing back; see the end of
                this section for why.
            </p>

            <p>
                The shipped configuration defines <b>no bridges and no cluster nodes</b>, so the
                Bridges page is empty on a fresh installation. Everything below is added by hand.
            </p>

            <p><b>Before you start</b></p>

            <ul>
                <li>Both servers installed, running, and reachable from each other on their MQTT
                    port - a bridge is an ordinary outbound MQTT connection, so a firewall between
                    the hosts needs that port open in both directions.
                </li>
                <li>Decide which topics to carry. This example uses <code>test/#</code>. Prefer a
                    specific prefix to <code>#</code>: it keeps the link's traffic to what you
                    actually meant to share.
                </li>
                <li>An account on each server for the other's bridge to log in as. The bridge
                    authenticates like any client, so it needs a real account on the
                    <em>remote</em> server. A non-administrator account is enough; create it on the
                    <b>Users</b> page if you do not already have one.
                </li>
            </ul>

            <p><b>Step 1 - add the bridge on mq-a</b></p>

            <p>
                Open <code>http://mq-a:18883</code>, sign in as an administrator, and go to the
                <b>Bridges</b> page. It is a master-detail page: the bridge list on top, and the
                settings of the selected bridge below. Press <b>Add</b>, fill the settings in, and
                press <b>Save Bridge</b>:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Field</th>
                    <th>Value on mq-a</th>
                    <th>Notes</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>Node name</td>
                    <td><code>mq-b</code></td>
                    <td>The name this server knows the remote by. It appears in the log and marks
                        messages that arrived over the link, which is how loops are prevented.
                        A name already used by another bridge is rejected.
                    </td>
                </tr>
                <tr>
                    <td>Host / port</td>
                    <td><code>mq-b</code> / <code>1883</code></td>
                    <td>The remote MQTT listener, not its configuration port. A second bridge to
                        the same host and port is rejected: it would carry every message twice.
                    </td>
                </tr>
                <tr>
                    <td>Mode</td>
                    <td><code>inout</code></td>
                    <td>Both directions.</td>
                </tr>
                <tr>
                    <td>Username</td>
                    <td>the account on <b>mq-b</b></td>
                    <td>Credentials for the remote server, not this one.</td>
                </tr>
                <tr>
                    <td>Password</td>
                    <td>its password</td>
                    <td></td>
                </tr>
                <tr>
                    <td>Client ID</td>
                    <td>leave empty</td>
                    <td>XMQ then derives a stable id from the two node names. Set one only if the
                        remote requires a particular id, and never one that varies.
                    </td>
                </tr>
                <tr>
                    <td>Enabled</td>
                    <td>ticked</td>
                    <td></td>
                </tr>
                <tr>
                    <td>Clean session</td>
                    <td>unticked</td>
                    <td>So mq-b keeps the subscriptions and queues messages while the link is down.</td>
                </tr>
                <tr>
                    <td>Encrypted</td>
                    <td>unticked</td>
                    <td>Tick it only when bridging to the remote's TLS port; it reveals the key
                        file fields, which are paths on this host.
                    </td>
                </tr>
                </tbody>
            </table>

            <p>
                Then fill the <b>Topics</b> table below the settings. Press its <b>Add</b> and give
                the topic a <b>Pattern</b> of <code>test/#</code>, a <b>Direction</b> of
                <code>inout</code>, and a <b>QoS</b> of 1. A bridge with no topics connects and
                carries nothing, so this table is not optional.
            </p>

            <p>
                Press <b>Save Bridge</b>. The bridge appears in the list with its topic count, and
                the configuration file is written immediately.
            </p>

            <p><b>Step 2 - nothing to do on mq-b</b></p>

            <p>
                mq-b needs no bridge of its own. It needs only the account the bridge signs in
                with, which it already has if you used one of its existing users. Nothing on mq-b
                records that a bridge exists: to it, mq-a's bridge is an ordinary client that
                subscribes and publishes.
            </p>

            <p><b>Step 3 - press Apply on each server</b></p>

            <p>
                This step is easy to miss. Saving a bridge stores it, but does <em>not</em> start
                the connection. <b>Apply</b>, on the Bridges page, rebuilds the connections from
                the configuration as it now stands: the bridges that were running are stopped, and
                the configured ones are started. Press it on <b>mq-a</b>.
            </p>

            <p>
                Neither server has to be restarted, and mq-b does not have to be up yet: a bridge
                whose remote is not answering retries on its own, the wait between attempts growing
                to half a minute and dropping back as soon as it connects.
            </p>

            <p>
                Restarting the server has the same effect, since bridges are started with it, and
                remains the way to apply a change made by editing the configuration file directly:
            </p>

            <pre className="userManualCode">{`sudo systemctl restart xmq_server`}</pre>

            <p><b>Step 4 - confirm the link came up</b></p>

            <p>
                With <b>Connections</b> at <code>DEBUG</code> on the <b>Logging</b> page, mq-a's
                log carries two lines for the bridge:
            </p>

            <pre className="userManualCode">{`Bridge to mq-b forwarding 1 outbound topic(s).
Bridge to mq-b (mq-b:1883) connected, 1 inbound topic(s).`}</pre>

            <p>
                Both counts matter. <code>connected</code> with <b>0 inbound topics</b> means the
                bridge reached the remote but carries nothing - almost always an empty or
                mistyped Topics field.
            </p>

            <p><b>Step 5 - prove a message crosses</b></p>

            <p>
                Subscribe on one server and publish on the other. The subscriber has no other way
                to see the message, so receiving it is itself the proof the bridge carried it:
            </p>

            <pre className="userManualCode">{`xmq_sub -h mq-b -p 1883 -u user -P secret -t test/bridge -q 1 -v -C 1 -W 15

xmq_pub -h mq-a -p 1883 -u user -P secret -t test/bridge -q 1 -m "over the bridge"`}</pre>

            <p>
                Then swap the two hosts and repeat, to check the other direction: one
                <code>inout</code> bridge carries both, but the two use different halves of it, and
                it is quite possible for one to work while the other does not.
            </p>

            <p>
                For a measured run rather than a single message, <code>xmq_scn</code> can publish
                to one server and subscribe on the other:
            </p>

            <pre className="userManualCode">{`xmq_scn -s Basic/Point-To-Point-1K-1K-1K-1K.json \\
        --host mq-a --port 1883 --subscriber-host mq-b --subscriber-port 1883`}</pre>

            <p>
                Every message it counts has crossed the bridge, so the reported latency includes
                the extra broker hop and the round trip between the hosts. Compare it against the
                same scenario run without <code>--subscriber-host</code> to see what the link costs.
            </p>

            <p><b>If nothing crosses</b></p>

            <ul>
                <li><b>No bridge line in the log at all</b> - <b>Apply</b> was not pressed after
                    the bridge was saved, or the bridge is not enabled. Saving stores the bridge;
                    it does not start it.
                </li>
                <li><b>Repeated connect failures</b> - the host or port is wrong, the port is
                    blocked, or the credentials name an account that does not exist on the
                    <em>remote</em> server. The log names the reason.
                </li>
                <li><b>Connected, nothing carried</b> - the topic pattern does not cover the topic
                    being published. Patterns are matched as MQTT subscriptions:
                    <code>test/#</code> covers <code>test/bridge</code>, while <code>test</code>
                    on its own does not.
                </li>
                <li><b>One direction only</b> - check the mode on both bridges. A bridge in
                    <code>out</code> mode forwards but never subscribes.
                </li>
                <li><b>Everything arrives twice</b> - a bridge is defined on both servers, each
                    covering the same topics. One <code>inout</code> bridge carries both
                    directions by itself, so the second one only moves the same message a second
                    way; remove it. The subscribers on the publishing server still see one copy,
                    which is what makes this look like a delivery fault rather than a
                    configuration one. This is explained under <i>Why one bridge, and not one on
                        each server</i> below.
                </li>
            </ul>

            <p><b>Why one bridge, and not one on each server</b></p>

            <p>
                An <code>inout</code> bridge already carries both directions on its own: it
                subscribes on the remote for what comes in, and subscribes locally for what goes
                out. A second bridge, defined on the other server and pointing back, adds nothing
                but a second copy - the message arrives once because this server pulled it, and
                again because the other server pushed it. Both copies are legitimate deliveries,
                so nothing detects or suppresses them: a subscriber simply receives everything
                twice.
            </p>

            <p>
                Define the bridge on one of the two servers only. Which one does not matter.
            </p>

            <p>
                Bridged traffic is not sent back the way it came - a message is marked with the
                node it arrived from, and a bridge subscription is never given a marked message.
                That marking is XMQ's own, so it only covers the bridges XMQ itself makes. When
                the broker at the other end is doing the bridging, what protects you is described
                next.
            </p>

            <h5>More than two servers</h5>

            <p>
                Bridged traffic is not passed on. A message that reached this server over one
                bridge is delivered to its own subscribers, but it is not handed to a second
                bridge - that is the same rule that stops a message going back where it came
                from, and it cannot tell "back" from "onward".
            </p>

            <p>
                So the shapes that look natural do not work. In a chain
                <code>A - B - C</code>, B sees everything from both, but A and C never see each
                other. In a star, the hub sees every spoke and no spoke sees another.
            </p>

            <p>
                Give every pair its own bridge instead. Three servers need three:
                <code>A-B</code>, <code>A-C</code> and <code>B-C</code>, each defined once, on
                either end. Every server then has a direct link to every other, so nothing needs
                relaying - and because nothing is relayed, nothing is duplicated either: each
                message arrives exactly once at each server.
            </p>

            <p>
                The cost is that the number of bridges grows as the square of the number of
                servers: three servers need three bridges, four need six, five need ten. Bridging
                suits a handful of servers, and the arithmetic is what limits it.
            </p>

            <h5>Bridging to a broker that is not XMQ</h5>

            <p>
                A bridge to Mosquitto, EMQX, or anything else speaking MQTT is configured exactly
                as above; only the remote's own settings differ. One of them is worth getting
                right, because it decides whether messages can circulate endlessly between the two
                brokers.
            </p>

            <p>
                <b>Configure the remote's bridge to use MQTT 5.</b> In Mosquitto that is one line
                in its bridge block:
            </p>

            <pre className="userManualCode">{`connection xmq
address xmq-host:1883
topic test/# both 0 "" ""
remote_username user
remote_password secret
bridge_protocol_version mqttv50`}</pre>

            <p>
                MQTT 5 defines two subscription options that exist for precisely this purpose.
                <b>No Local</b> tells the broker not to send a subscriber back what that same
                connection published, which is what stops a message going round; and <b>Retain As
                Published</b> keeps a retained message retained as it crosses. A bridge that
                subscribes with them cannot be echoed to, and XMQ honours both, whoever set them.
                XMQ's own bridges set them too, which is why a bridge between two XMQ servers needs
                nothing configured for this.
            </p>

            <p>
                Mosquitto defaults to <code>mqttv311</code> instead. Such a bridge still connects
                and still carries messages, but the loop protection is absent: MQTT 3.1.1 has no
                subscription options to carry it, and the non-standard "bridge" protocol variant
                Mosquitto tries first - a protocol level with the high bit set - is not something
                XMQ accepts. Mosquitto notices the refusal and reconnects as an ordinary client,
                so the only visible cost is one rejected connection attempt, and the invisible one
                is that XMQ cannot tell that client is a bridge.
            </p>

            <p>
                With a 3.1.1 remote bridge, then, keep the topic patterns from overlapping in both
                directions - carry <code>a/#</code> one way and <code>b/#</code> the other, rather
                than <code>#</code> both ways - or drive the link entirely from the XMQ side, where
                the origin marking applies.
            </p>

            <h5>Server limits</h5>

            <p>
                The <b>Server Limits</b> page holds the thread counts and the per-session bounds.
                The rule for threads is short: XMQ pins its threads to physical cores, and no thread
                group should be larger than the host's physical core count. The groups share those
                cores, so count the send threads, the receive threads, the persistence threads and
                the per-listener threads together rather than each on its own. Adding threads past
                that buys contention, not throughput.
            </p>
        </div>;
    }

    renderTestingSuite() {
        return <div>
            <h4 id="testing-suite">MQTT testing suite</h4>

            <p>
                <code>xmq_scn</code> runs load-test scenarios described by JSON files. It is
                broker-neutral: it speaks plain MQTT and can be pointed at Mosquitto, EMQX, NanoMQ
                or anything else, which is what makes its numbers comparable across brokers.
            </p>

            <p>
                Scenarios ship in <code>/usr/local/share/xmq</code>. List them with:
            </p>

            <pre className="userManualCode">{`xmq_scn --list-scenarios`}</pre>

            <p>
                A scenario file names the client groups, their counts, the topics, and the pacing.
                The shipped set covers connection ramps, fan-in, fan-out, and point-to-point, at
                sizes from 10,000 to 1,000,000 connections. Writing a new one is a matter of copying
                the closest and changing the numbers.
            </p>

            <pre className="userManualCode">{`xmq_scn -s Point-To-Point-50K-50K-50K-50K.json --progress
xmq_scn -s 1M-Connections-5K-rate.json -h broker -p 1883`}</pre>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Option</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>-s</code>, <code>--scenario</code></td>
                    <td>Scenario file. A relative path is looked up in the current directory first,
                        then in the installed scenario directory
                    </td>
                </tr>
                <tr>
                    <td><code>-m</code>, <code>--payload-size</code></td>
                    <td>Message size, overriding the scenario</td>
                </tr>
                <tr>
                    <td><code>-r</code>, <code>--publish-rate</code></td>
                    <td>Messages per second per publisher. 0 means unpaced: as fast as possible</td>
                </tr>
                <tr>
                    <td><code>-d</code>, <code>--duration</code></td>
                    <td>How long to run</td>
                </tr>
                <tr>
                    <td><code>-q</code>, <code>--qos</code></td>
                    <td>Quality of service to publish with</td>
                </tr>
                <tr>
                    <td><code>--max-inflight</code></td>
                    <td>Cap on a publisher's un-acknowledged QoS 1 backlog, so it throttles against
                        its own round-trip time rather than publishing blindly at the configured rate
                    </td>
                </tr>
                <tr>
                    <td><code>--id-prefix</code></td>
                    <td>Client id prefix, so several client hosts don't collide</td>
                </tr>
                <tr>
                    <td><code>--progress</code></td>
                    <td>Show a progress bar</td>
                </tr>
                </tbody>
            </table>

            <p>
                Reaching the larger sizes needs the tuning above on both hosts, and enough source
                addresses on the client: a connection is identified by its source address and port,
                so one address runs out of ports long before a million connections. The tested
                1,000,000-connection runs used 30 client addresses, about 33,000 connections each.
                The <code>make_ip_addresses.sh</code> helper in the same directory adds them, and
                <code>remove_ip_addresses.sh</code> takes them away again:
            </p>

            <pre
                className="userManualCode">{`./make_ip_addresses.sh -s <first three octets> -p <prefix> -f <first host octet> -c 29`}</pre>

            <p>
                Scenarios spread their clients across whatever addresses the host has, so the
                <code>--id-prefix</code> option keeps client ids distinct when several client
                machines drive the same broker.
            </p>

            <div className="userManualNote">
                Run the load generator on a different machine from the broker. Sharing a host means
                measuring the two competing for the same cores, which flatters neither.
            </div>
        </div>;
    }

    renderTestingSetup() {
        return <div>
            <h4 id="testing-setup">Testing your setup</h4>

            <p>
                Three small utilities are enough to prove an installation works. All of them take
                <code>-h</code> host, <code>-p</code> port, <code>-u</code> username,
                <code>--password</code>, and <code>--version</code>.
            </p>

            <h5>Is the server up?</h5>
            <pre className="userManualCode">{`systemctl status xmq_server
tail -f /var/log/xmq/xmq_server.log`}</pre>

            <p>
                The log names each listener it opened at startup, and says which storage it is
                using: a Redis address, or <code>memory</code> when persistence is off or Redis
                could not be reached.
            </p>

            <h5>Subscribe and publish</h5>

            <p>
                In one terminal, subscribe. In another, publish, and watch it arrive:
            </p>

            <pre className="userManualCode">{`xmq_sub -h localhost -p 1883 -u user --password secret -t "test/#" -q 1 -v

xmq_pub -h localhost -p 1883 -u user --password secret -t test/hello -m "it works"`}</pre>

            <p>
                Useful <code>xmq_pub</code> options: <code>-r</code> repeats the message a number of
                times, <code>-g</code> paces it to a rate, <code>-f</code> takes the payload from a
                file, <code>-s</code> reads it from standard input, and <code>-l</code> stamps a
                send timestamp into the payload so latency can be measured at the far end.
            </p>

            <h5>Testing TLS</h5>
            <pre className="userManualCode">{`xmq_sub -h localhost -p 8883 -u user --password secret \\
        -t "test/#" -q 1 -v --cafile /etc/xmq/certs/ca.crt`}</pre>

            <p>
                If this fails while the plain port works, the certificates are the place to look:
                the server log says whether it managed to load them at startup.
            </p>

            <h5>Many connections at once</h5>

            <p>
                <code>xmq_con</code> opens a number of sessions and holds them, which is the quickest
                way to see whether the host limits are in place:
            </p>

            <pre
                className="userManualCode">{`xmq_con -h localhost -p 1883 -u user --password secret -n 10000 --show-counters`}</pre>

            <p>
                If it stops short of the number asked for, the file descriptor limit is the usual
                cause, on whichever host ran out first. Check both, then see the tuning section.
            </p>

            <h5>Checking a bridge</h5>

            <p>
                Subscribe on the remote broker, publish on XMQ, and confirm the message crosses:
            </p>

            <pre className="userManualCode">{`xmq_sub -h remote-broker -p 1883 -t "bridged/#" -q 1 -v

xmq_pub -h localhost -p 1883 -u user --password secret -t bridged/test -m "over the bridge"`}</pre>

            <p>
                Nothing arriving usually means the bridge is disabled, its topic pattern does not
                cover the topic, or its direction excludes it. The server log, with
                <b>Connections</b> at <code>DEBUG</code>, shows the bridge connecting and how many
                topics it subscribed to.
            </p>
        </div>;
    }

    render() {
        return <div className="userManual">
            <h3 style={{textAlign: 'Left'}}>XMQ User Manual</h3>

            <p>
                XMQ is an MQTT 5, 3.1.1, and 3.1 server. This manual covers installing it, tuning
                the host it runs on, configuring it from the configuration interface, and proving that the
                result works.
            </p>

            {this.renderContents()}
            {this.renderSupportedOses()}
            {this.renderInstall()}
            {this.renderTuning()}
            {this.renderBasicSetup()}
            {this.renderAdvancedSetup()}
            {this.renderTestingSuite()}
            {this.renderTestingSetup()}
        </div>;
    }
}
