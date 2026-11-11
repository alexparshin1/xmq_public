import React from "react";
import {NavLink} from "react-router-dom";
import './AccordionGroup.css';

/**
 * @brief One group of the main menu, rendered in the style of the XMQ web site.
 *
 * The group is a header followed by its items, shown only while the group is expanded. The menu
 * keeps a single group expanded, so this component is told whether it is the expanded one rather
 * than owning that decision: an accordion with two open groups is what makes a long menu
 * unreadable in the first place.
 *
 * Props:
 * - title: the group header
 * - items: an object of link -> label, in display order
 * - expanded: whether the items are shown
 * - selectedLink: the link of the current page, highlighted the way the site highlights it
 * - onHeaderClick: called when the header is clicked
 */
export default function AccordionGroup({title, items, expanded, selectedLink, onHeaderClick}) {
    const groupClass = expanded ? "AccordionGroup AccordionGroupSelected" : "AccordionGroup";
    const nodes = [];
    for (const link in items) {
        const itemClass = link === selectedLink ? "AccordionItemSelected" : "AccordionItem";
        nodes.push(
            <div key={"group-item-" + link} className={itemClass}>
                <NavLink to={link}>{items[link]}</NavLink>
            </div>
        );
    }

    return <div className="AccordionGroupBlock">
        <button key={"title-" + title} type="button" className={groupClass}
                aria-expanded={expanded} onClick={onHeaderClick}>{title}</button>
        {expanded ? <div key={"group-" + title} className="AccordionGroupItems">{nodes}</div> : null}
    </div>;
}
