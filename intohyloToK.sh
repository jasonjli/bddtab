cat $1 | head -n -1 | tail -n +2 | \
		sed -e 's/<->/<=>/g' -e 's/->/=>/g' -e 's/<r1>/<>/g' -e 's/\[r1\]/[]/g' -e 's/^/~(/g' -e 's/$/)/g' -e 's/false/False/g' -e 's/true/True/g' > "$1.k"
