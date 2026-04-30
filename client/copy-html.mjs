// Copies index.html into dist/ alongside the bundled main.js so the
// `dist/` directory is self-contained and can be served by any static
// HTTP server (or opened directly via file://).
import { copyFileSync, mkdirSync } from "node:fs";
mkdirSync("dist", { recursive: true });
copyFileSync("index.html", "dist/index.html");
console.log("copied index.html → dist/index.html");
