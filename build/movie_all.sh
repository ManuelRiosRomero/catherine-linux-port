#!/bin/bash
# Run the movie self-test over every cutscene in the given data/ subfolders (default: movie); summary at the end.
. "$(dirname "$0")/paths.sh"; cd "$REPO"
D="$GAME/data"
[ $# -gt 0 ] || set -- movie
files=(); for d in "$@"; do files+=("$D/$d/"*.wmv); done
bash runtime/movie_test.sh "${files[@]}" 2>gen/movie_test/stderr.txt | tee gen/movie_test/all.txt
echo "matched $(grep -c ^MATCH gen/movie_test/all.txt) / $(wc -l < gen/movie_test/all.txt), rewind ok $(grep -c 'rewind ok' gen/movie_test/stderr.txt)"
