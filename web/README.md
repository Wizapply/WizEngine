# web/

Static files served by the built-in HTTP server.

At build time this folder is copied to `build/assets/web/`, which is where the
server reads it from at runtime (everything the program loads lives under
`assets/`).

- `index.html` — markup only
- `style.css` — all styles
- `app.js` — all logic (classic script at the end of `<body>`; top-level
  functions must stay global because the inline `onclick=` handlers use them)

- `favicon.ico` — browser tab icon. Optional; drop your own file here and
  re-run `cmake -B build` so the copy step picks it up. Any format the browser
  accepts works (`.ico`, `.png`, `.svg`) — if you use a different extension,
  update `ICON_SRC` in `CMakeLists.txt` and the filename passed to
  `HttpServer` in `src/main.cpp`.
