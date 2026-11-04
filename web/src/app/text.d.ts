// Plain-text files bundled into the page as strings (esbuild "text" loader, build.mjs).
declare module '*.txt' { const text: string; export default text; }
// The page's stylesheet, bundled by esbuild into dist/main.css.
declare module '*.css';
