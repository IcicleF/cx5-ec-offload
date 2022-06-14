.PHONY: all clean

all: test

test: test.cpp
	g++ -o test test.cpp -lisal -libverbs

clean:
	rm -f ./test
