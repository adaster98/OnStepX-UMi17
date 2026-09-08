# Host test for the flash journal. Compiles the real Journal.cpp - unmodified,
# straight out of the tree - against a fake flash that enforces NOR semantics.
#
#   make        build and run
#   make clean

SRC := ../../src/lib/journal
CXXFLAGS := -O1 -g -Wall -I. -fsanitize=address,undefined

run: jsim
	./jsim

# Journal.h includes "../../Common.h"; the local shim stands in for it, so the
# copies here are byte-identical to the tree and stay honest.
Journal.h: $(SRC)/Journal.h
	sed 's#\.\./\.\./Common\.h#Common.h#' $< > $@

Journal.cpp: $(SRC)/Journal.cpp
	cp $< $@

jsim: test.cpp flash.cpp Journal.cpp Journal.h Common.h esp_partition.h
	$(CXX) $(CXXFLAGS) -o $@ test.cpp flash.cpp Journal.cpp

clean:
	rm -f jsim Journal.cpp Journal.h

.PHONY: run clean
