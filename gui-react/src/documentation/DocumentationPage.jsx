import React from "react";
import {documentationPage} from "./pages";
import "./Documentation.css";

export default function DocumentationPage({page, children}) {
    return <article className="userManual">
        <h1>{documentationPage(page).title}</h1>
        {children}
    </article>;
}
