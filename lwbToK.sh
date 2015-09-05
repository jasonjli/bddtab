zcat $1 | head -n -1 | tail -n +3 | \
		sed -e 's/<->/<=>/g' -e 's/->/=>/g' -e 's/box/[]/g' -e 's/dia/<>/g' -e 's/ v / | /g' -e 's/^[^:]*://g' -e 's/false/False/g' -e 's/true/True/g' > $1.k
