import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function InstallDocumentation() {
    return <DocumentationPage page="install">
            <section>
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
            </section>
            <section>
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

            <pre className="userManualCode">{`sudo -u xmq xmq_server --set-password admin`}</pre>

            <p>
                It asks for the password without showing it, and from a pipe reads standard input
                instead, so the password never appears in the process list or the shell history.
                On a headless server the alternative is to forward the port &mdash;
                <code>ssh -L 18883:127.0.0.1:18883 the-server</code> &mdash; and use the interface
                as usual. In a Docker container, run it with{" "}
                <code>docker exec -it &lt;container&gt; xmq_server --set-password admin</code>, then
                restart the container; keep <code>/etc/xmq</code> on a volume and it stays set.
            </p>

            <p>
                Once the server runs, the configuration interface is reachable on port 18883 by
                default. On a server that has just been installed it answers on that machine only,
                at <code>https://127.0.0.1:18883</code>, and the <code>admin</code> account it
                created has no password: the interface signs the administrator in by itself and
                opens the <b>Initial Setup</b> page, and the password set there is what opens
                the interface to the rest of the network.
            </p>

            <div className="userManualNote">
                Nothing is shipped with a password. Until the administrator has one, the account
                cannot be used over MQTT at all and the interface cannot be reached from another
                machine &mdash; so the first sign-in has to be made from the server&apos;s own
                console, or through a forwarded port.
            </div>
            </section>
            <section>
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

# /etc/udev/rules.d/60-rps.rules
ACTION=="add", SUBSYSTEM=="net", KERNEL=="eth0", ATTR{queues/rx-0/rps_cpus}="70", ATTR{queues/rx-0/rps_flow_cnt}="32768"`}</pre>

            <p>
                Or with a systemd unit, which needs no interface name: it spreads every receive queue of
                every wired card over all CPUs at boot. It suits a small machine with one card - a
                cluster node on such a host answered in 3.2 ms without it and in 0.42 ms with it, at
                50,000 messages a second.
            </p>

            <pre className="userManualCode">{`# /etc/systemd/system/rps.service
[Unit]
Description=Spread received packets over all CPUs (RPS) on NICs with a single receive queue
After=network-pre.target
Before=network.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'mask=$(printf "%%x" $(( (1 << $(nproc)) - 1 ))); for q in /sys/class/net/e*/queues/rx-*/rps_cpus; do echo $mask > $q; done'
ExecStop=/bin/sh -c 'for q in /sys/class/net/e*/queues/rx-*/rps_cpus; do echo 0 > $q; done'

[Install]
WantedBy=multi-user.target`}</pre>

            <pre className="userManualCode">{`sudo systemctl daemon-reload
sudo systemctl enable --now rps.service
cat /sys/class/net/*/queues/rx-0/rps_cpus      # every CPU's bit set
sudo systemctl disable --now rps.service       # back to one CPU`}</pre>

            <p>
                Other systems have their own equivalents, which XMQ does not check. On FreeBSD it is
                netisr: <code>net.isr.maxthreads</code> and <code>net.isr.bindthreads</code> in
                <code>/boot/loader.conf</code>, with <code>net.isr.dispatch=deferred</code>. On Windows
                it is receive side scaling, set with <code>Set-NetAdapterRss</code>, and available only
                when the card's driver supports several queues.
            </p>
            </section>
    </DocumentationPage>;
}
