Vendored from https://github.com/lieff/minimp3 (CC0-1.0 / public domain).

`minimp3.h` and `minimp3_ex.h` are not committed. `cmake/vendored.cmake`
fetches them at configure time, pinned to a commit and checked against a
SHA256, so a plain `idf.py build` is enough. `./tools/fetch_vendored.sh`
does the same thing by hand if you want them on disk first.

Do not commit them once fetched -- they are gitignored deliberately.

Why this is not an `idf_component.yml` dependency: upstream ships no
component manifest and there is no first-party registry entry, so the
alternative is a third-party mirror of unknown maintenance. Two headers
are cheaper to audit than that.
