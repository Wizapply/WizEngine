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
  rebuild so the `webui` copy step picks it up (the whole folder is copied,
  so no CMake edit is needed). The server serves `/favicon.ico` from this
  file; for another format keep serving it under that name or add a
  `<link rel="icon">` to `index.html`.
