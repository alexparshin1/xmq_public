// Shared by the broker's Documentation menu and the website's documentation pages.
export const documentationPages = [
    {id: "index", label: "Index", path: "/xmq_documentation",
        title: "XMQ MQTT Server Documentation",
        description: "Installation, configuration, testing, and setting up an XMQ cluster.", anchors: []},
    {id: "install", label: "Install", path: "/xmq_doc_install",
        title: "Download and Install",
        description: "Supported systems, packages, dependencies and host tuning.",
        anchors: ["supported-oses", "install", "tuning"]},
    {id: "setup", label: "Setup", path: "/xmq_doc_setup",
        title: "Basic and Advanced Setup",
        description: "Listeners, TLS, persistence, authentication, bridges, logging and limits.",
        anchors: ["basic-setup", "advanced-setup"]},
    {id: "testing", label: "Testing", path: "/xmq_doc_testing",
        title: "MQTT Test Suite and Smoke Tests",
        description: "The built-in scenario engine, and proving a new installation.",
        anchors: ["testing-suite", "testing-setup"]},
    {id: "cluster", label: "Cluster", path: "/xmq_doc_cluster",
        title: "Setting up a Cluster",
        description: "Shared accounts and Redis, node certificates, and joining a cluster.",
        anchors: ["cluster-setup"]}
];

export function documentationPage(id) {
    return documentationPages.find(page => page.id === id);
}
