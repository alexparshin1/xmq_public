import React, {useEffect} from "react";
import {useLocation} from "react-router-dom";
import {documentationPage} from "./pages";
import "./Documentation.css";

export default function DocumentationPage({page, children}) {
    const {hash} = useLocation();
    useEffect(() => {
        if (hash) document.getElementById(hash.slice(1))?.scrollIntoView?.();
    }, [page, hash]);

    return <article className="userManual">
        <h1>{documentationPage(page).title}</h1>
        {children}
    </article>;
}
