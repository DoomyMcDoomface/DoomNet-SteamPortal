# Millennium's `json` module source

`json.c` is Millennium's own single-file lua-cjson fork, copied unchanged except
for the one edit below.

- Upstream: https://github.com/SteamClientHomebrew/Millennium
  `src/lua_host/api/json.c` on `main`, last changed in commit
  `c05144b98f7e05242d03b251184f60bc472d1023` (2026-05-06).
- License: MIT (Mark Pulford's lua-cjson terms, in the header of the file).

We use this file, not upstream mpx/lua-cjson, because the player's saved files
(`library_index.json`, `settings.json`, playlists) were written by it and the
page parses what it returns. It differs from stock cjson in ways that change
output bytes:

- `encode_empty_table_as_object` is on: `json.encode({})` is `{}`.
- `encode_escape_forward_slash` is on: `/` is written as `\/`.
- `decode_invalid_numbers` is turned off (`DISABLE_INVALID_NUMBERS` block).
- `encode_number_precision` is 14, sparse arrays 0 / 2 / 10, depth 1000.

## Local edit

1. `CJSON_EXPORT` is only defined when the build has not defined it.
   `CMakeLists.txt` defines it as `extern` so the static library does not add an
   export table to `DoomNet.exe`.

Do not change the defaults. Add a known input and output to a test first.
