# web/

Static files served by the built-in HTTP server.

At build time this folder is copied to `build/assets/web/`, which is where the
server reads it from at runtime (everything the program loads lives under
`assets/`).

- `favicon.ico` — browser tab icon. Optional; drop your own file here and
  re-run `cmake -B build` so the copy step picks it up. Any format the browser
  accepts works (`.ico`, `.png`, `.svg`) — if you use a different extension,
  update `ICON_SRC` in `CMakeLists.txt` and the filename passed to
  `HttpServer` in `src/main.cpp`.
