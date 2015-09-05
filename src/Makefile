all: compile

compile: bddtab.cpp
	g++ -Wall -std=c++0x -O2 -o ../bddtab bddtab.cpp KFormula.cpp -Wl,-Bstatic -lbdd -Wl,-Bdynamic
