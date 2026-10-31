#include "NodeCommand.h"
#include "Cluster/NodeIdentity.h"
#include "common/DirectoryNames.h"
#include "server/Settings/Settings.h"
#include "service/CServerConfiguration.h"
#include "service/CClusterControl.h"
#include "service/CClusterControlResponse.h"
#include "service/CLogin.h"
#include "service/CLoginResponse.h"
#include <sptk5/net/HttpConnect.h>
#include <sptk5/net/SSLSocket.h>
#include <fstream>
#include <thread>

using namespace std;
using namespace sptk;

String xmq::controlNodeCluster(const filesystem::path& configuration, bool join,
                               const String& username, const String& password,
                               chrono::seconds timeout)
{
    if (timeout <= chrono::seconds::zero()) throw Exception("Node command timeout must be positive.");
    const auto file = DirectoryNames::resolveConfigurationFile(configuration);

    // Loaded as the server loads it, because the port this command needs may not be in the file at
    // all: for a node whose settings live in a database, the web interface's port is the node's
    // row. Reading the file alone found no port and refused to run.
    Settings settings;
    settings.loadConfiguration(file);
    string uuid;
    ifstream identity(file.parent_path() / cluster::NodeIdentity::FileName);
    getline(identity, uuid);
    if (!cluster::NodeIdentity::isGuid(uuid))
        throw Exception("Start this node first: its UUID file is missing or invalid.");
    const auto port = settings.m_web_service.m_listener_port.asInteger();
    if (port < 1 || port > 65535) throw Exception("No usable web interface port in configuration.");
    const bool encrypted = settings.m_web_service.m_encrypted.isNull() || settings.m_web_service.m_encrypted.asBool();
    auto certificate = filesystem::path(settings.m_web_service.m_certfile.asString().c_str());
    if (certificate.empty()) certificate = file.parent_path() / "certs/node.crt";
    // Resolve the configuration placeholder without loading Settings (which can write SQL).
    if (const auto text = certificate.generic_string(); text.starts_with("${ProgramCerts}/"))
        certificate = file.parent_path() / "certs" / text.substr(16);
    const auto deadline = chrono::steady_clock::now() + timeout;
    const auto post = [&](const char* method, const WSComplexType& input,
                          WSComplexType& output, const String& token = String())
    {
        const auto remaining = chrono::duration_cast<chrono::milliseconds>(deadline - chrono::steady_clock::now());
        if (remaining <= chrono::milliseconds::zero()) throw Exception("Node command timed out.");
        const auto requestTimeout = min(remaining, chrono::milliseconds(5000));
        shared_ptr<TCPSocket> socket;
        if (encrypted)
        {
            auto tls = make_shared<SSLSocket>();
            tls->loadKeys(SSLKeys({}, {}, "", certificate, 1, 1));
            socket = tls;
        }
        else socket = make_shared<TCPSocket>();
        socket->open(Host("127.0.0.1", static_cast<uint16_t>(port)), Socket::OpenMode::CONNECT,
                     true, requestTimeout);
        HttpConnect http(socket);
        http.requestHeaders()["Content-Type"] = "application/json";
        xdoc::Document request;
        input.unload(request.root());
        Buffer body, response;
        request.exportTo(xdoc::DataFormat::JSON, body);
        const HttpConnect::BearerAuthorization auth(token);
        const auto status = http.cmd_post(String("/") + method, {}, body, response, {},
                                          token.empty() ? nullptr : &auth, requestTimeout);
        if (status != 200) throw Exception("Local control API returned HTTP " + to_string(status));
        xdoc::Document result;
        result.load(response);
        output.load(result.root());
    };
    CLogin login;
    login.m_username = username;
    login.m_password = password;
    CLoginResponse loggedIn;
    post("Login", login, loggedIn);
    if (!loggedIn.m_result.m_success.asBool())
        throw Exception("Local control API authentication failed: " + loggedIn.m_result.m_description.asString());
    const String token = loggedIn.m_token.asString();
    CClusterControl request;
    request.m_action = "get";
    CClusterControlResponse status;
    const auto get = [&]
    {
        CClusterControlResponse current;
        post("ClusterControl", request, current, token);
        if (!current.m_result.m_success.asBool()) throw Exception(current.m_result.m_description.asString());
        if (current.m_this_node.m_node_id.asString() != uuid)
            throw Exception("The web interface belongs to a different node; check -c and its web port.");
        return current;
    };
    status = get();
    if (join && !status.m_on_cluster_database.asBool())
        throw Exception("Joining requires the shared PostgreSQL or MySQL accounts database.");
    const auto complete = [&](const CClusterControlResponse& current)
    {
        if (!join)
        {
            if (current.m_enabled.asBool() || current.m_this_node.m_state.asString() != "Standalone") return false;
            for (const auto& member: current.m_cluster_members)
                if (member.m_node_id.asString() == uuid) return false;
            return true;
        }
        if (!current.m_enabled.asBool()) return false;
        for (const auto& member: current.m_cluster_members)
            if (member.m_node_id.asString() == uuid && member.m_is_alive.asBool()) return true;
        return false;
    };
    if (!complete(status))
    {
        CClusterControl action;
        action.m_action = join ? "join" : "disable";
        action.m_node.m_node_id = uuid;
        CClusterControlResponse accepted;
        post("ClusterControl", action, accepted, token);
        if (!accepted.m_result.m_success.asBool()) throw Exception(accepted.m_result.m_description.asString());
        String lastError;
        while (chrono::steady_clock::now() < deadline)
        {
            this_thread::sleep_for(chrono::milliseconds(200));
            try
            {
                status = get();
                if (complete(status)) return join ? "Node joined the cluster." : "Node left the cluster and is Standalone.";
                lastError = status.m_this_node.m_last_error.asString();
                if (status.m_this_node.m_state.asString() == "Failed") break;
            }
            catch (const Exception& error) { lastError = error.message(); }
        }
        throw Exception("Cluster operation was requested but completion was not confirmed: " + lastError);
    }
    return join ? "Node is already a live cluster member." : "Node is already Standalone.";
}
