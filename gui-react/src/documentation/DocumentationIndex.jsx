import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";
import {documentationPages} from "./pages";

export default function DocumentationIndex() {
    return <DocumentationPage page="index">
        <p>
            The manual covers installing XMQ, tuning the host it runs on, configuring it from
            the configuration interface, proving that the result works, and setting up a cluster.
            Each page is self-contained.
        </p>
        <table className="userManualTable">
            <thead><tr><th>Page</th><th>What it covers</th></tr></thead>
            <tbody>
                {documentationPages.filter(page => page.id !== "index").map(page =>
                    <tr key={page.id}>
                        <td><Link to={page.path}>{page.title}</Link></td>
                        <td>{page.description}</td>
                    </tr>)}
            </tbody>
        </table>
    </DocumentationPage>;
}
