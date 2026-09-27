import React from "react";
import {NavLink} from "react-router-dom";
import './AccordionGroup.css';

export default class AccordionGroup extends React.Component {

    state = {
        collased: false
    };

    /**
     * Constructs a new MainMenu component.
     * Calls the superclass constructor and hooks the change of connection status to ControlAPI.
     * When the connection status changes, the component's state is updated to reflect the new status.
     * @memberof MainMenu
     */
    constructor(props) {
        super();
        this.items = props.items;
        this.title = props.title;
        this.state.collapsed = props.collapsed;
    }

    setCollapsed(isCollapsed) {
        this.setState({collapsed: isCollapsed});
    }

    renderGroupItem(link, text) {
        return <div key={"group-item-" + link} className="AccordionGroupItem">
            <NavLink key={"group-link-" + link} to={link}>{text}</NavLink>
        </div>;
    }

    render() {
        let groupItems = [];
        for (let link in this.items) {
            groupItems.push(this.renderGroupItem(link, this.items[link]));
        }

        return <div className="AccordionGroup" style={{align: 'left'}}>
            <div key={"title-" + this.title} className="AccordionGroupTitle">{this.title}</div>
            <div key={"group-" + this.title} className="AccordionGroupItems">
                {groupItems}
            </div>
        </div>;
    }
}
