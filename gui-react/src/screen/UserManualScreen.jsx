import React from "react";
import {Navigate, useLocation} from "react-router-dom";
import {documentationPages} from "../documentation/pages";

// Keep links to the former single-page manual, including its section bookmarks, working.
export default function UserManualScreen() {
    const {hash} = useLocation();
    const anchor = hash.slice(1);
    const page = documentationPages.find(entry => entry.anchors.includes(anchor)) || documentationPages[0];
    return <Navigate to={page.path + (page.anchors.includes(anchor) ? hash : "")} replace/>;
}
