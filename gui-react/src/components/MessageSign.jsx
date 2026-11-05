import React from "react";

/**
 * The signs, drawn rather than loaded.
 *
 * Inline SVG so that a message can be shown before anything else has finished loading - which is
 * exactly when things go wrong - and so that the drawing scales with the box rather than blurring.
 * One set for every window that carries a sign, so an error in a hint and an error in a message
 * cannot read as two different things.
 */
const signs = {
    error: <svg viewBox="0 0 64 64" role="img" aria-label="Stop">
        <polygon points="20,2 44,2 62,20 62,44 44,62 20,62 2,44 2,20"
                 fill="#c62828" stroke="#8e0000" strokeWidth="2"/>
        <text x="32" y="40" textAnchor="middle" fontSize="17" fontWeight="bold"
              fontFamily="sans-serif" fill="white">STOP</text>
    </svg>,

    warning: <svg viewBox="0 0 64 64" role="img" aria-label="Warning">
        <polygon points="32,3 61,57 3,57" fill="#f9a825" stroke="#b06e00" strokeWidth="2"/>
        <text x="32" y="49" textAnchor="middle" fontSize="36" fontWeight="bold"
              fontFamily="sans-serif" fill="white">!</text>
    </svg>,

    question: <svg viewBox="0 0 64 64" role="img" aria-label="Question">
        <circle cx="32" cy="32" r="30" fill="#1565c0" stroke="#0d47a1" strokeWidth="2"/>
        <text x="32" y="46" textAnchor="middle" fontSize="42" fontWeight="bold"
              fontFamily="serif" fill="white">?</text>
    </svg>,

    information: <svg viewBox="0 0 64 64" role="img" aria-label="Information">
        <circle cx="32" cy="32" r="30" fill="#2e7d32" stroke="#1b5e20" strokeWidth="2"/>
        <text x="32" y="47" textAnchor="middle" fontSize="42" fontWeight="bold"
              fontFamily="serif" fill="white">i</text>
    </svg>
};

/**
 * One sign, beside a message or a hint.
 *
 * @param {Object} props
 * @param {string} props.kind   error, warning, question or information.
 * @param {number} [props.size] Side of the square in pixels; 64 unless a smaller box needs less.
 */
export default function MessageSign({kind, size = 64}) {
    return <span style={{display: "inline-block", width: size, height: size}}>
        {signs[kind] || signs.information}
    </span>;
}
