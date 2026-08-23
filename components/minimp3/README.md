Vendored from https://github.com/lieff/minimp3 (CC0-1.0 / public domain).

`minimp3.h` and `minimp3_ex.h` are not in this repository until you run:

    ./tools/fetch_vendored.sh

Commit them once fetched. They are build inputs, not build output.

Why this is not an `idf_component.yml` dependency: upstream ships no
component manifest and there is no first-party registry entry, so the
alternative is a third-party mirror of unknown maintenance. Two headers
are cheaper to audit than that.
