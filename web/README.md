# web/

Static files served by the built-in HTTP server.

At build time this folder is copied to `build/assets/web/`, which is where the
server reads it from at runtime (everything the program loads lives under
`assets/`).

- `index.html` — markup only
- `style.css` — all styles
- `app.js` — all logic (classic script at the end of `<body>`; top-level
  functions must stay global because the inline `onclick=` handlers use them)
- `terrain.js` — the terrain section of the Inspector (`secTerrain`, shown
  when a terrain is selected): the `<terrain>` fields and their partial
  updates (loaded right after `app.js` and uses its top-level helpers; same
  classic-script rules)
- `terrain-layers.js` — the paint layer rows of the terrain section (field
  table, row building, keeping just-sent values from being overwritten by
  polling) (loaded after `terrain.js` and uses the helpers of both; same
  classic-script rules)
- `terrain-tools.js` — the one-shot terrain tools (noise generation, undo,
  auto paint, heightmap import / export) (loaded after `terrain-layers.js`
  and uses the helpers of `app.js` and `terrain.js`; same classic-script
  rules)
- `terrain-files.js` — the terrain tiles of the asset panel (rename, context
  menu, new / open / duplicate / delete, list refresh) (loaded after
  `terrain-tools.js` and uses the helpers of `app.js` and `terrain.js`;
  same classic-script rules)
- `terrain-brush.js` — the terrain brush toolbar (`#terrainBar`) and the
  brush pointer input over the video (loaded right after `terrain-files.js`
  and uses the helpers of `app.js` and `terrain.js`; same classic-script rules)

  Script order at the end of `index.html`: `app.js` → `terrain.js` →
  `terrain-layers.js` → `terrain-tools.js` → `terrain-files.js` →
  `terrain-brush.js`. Statements that run at load time must not touch a
  later file (its `const` / `let` is not defined yet), and a top-level
  `const` / `let` name must not appear in two files.

- `favicon.ico` — browser tab icon. Optional; drop your own file here and
  rebuild so the `webui` copy step picks it up (the whole folder is copied,
  so no CMake edit is needed). The server serves `/favicon.ico` from this
  file; for another format keep serving it under that name or add a
  `<link rel="icon">` to `index.html`.
