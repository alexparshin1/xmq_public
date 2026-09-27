import React from "react";

/**
 * A file picker that reads the chosen file and hands its name and content to the screen.
 * The file is read in the browser: nothing is uploaded until the screen saves.
 */
export default class FileLoad extends React.Component
{
    constructor(props)
    {
        super(props);
        this.onLoad = props.onLoad;
        this.accept = props.accept;
    }

    handleFileSelect(event)
    {
        const files = event.target.files;
        if (!files || files.length === 0)
        {
            return;
        }

        // Only the first file: each picker stands for one certificate or key.
        const file = files[0];
        const reader = new FileReader();

        reader.onload = loaded => {
            // The file's own name, not the input's value: browsers report the latter as a
            // fake path, and the state set alongside it wouldn't be applied yet anyway.
            this.onLoad(file.name, loaded.target.result);
        };

        reader.readAsText(file);
    }

    render()
    {
        // Deliberately uncontrolled: a file input rejects having its value set.
        return <input type="file" accept={this.accept}
                      onChange={(e) => this.handleFileSelect(e)}/>;
    }
}
