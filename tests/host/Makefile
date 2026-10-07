# Host-side simulation of the Cycle Tester sketch. No Arduino or hardware needed: just g++ and Python 3.
#
#   make            build and run the full simulation (takes a few seconds)
#   make pictures   also turn the saved screens in out/ into PNG files (needs Pillow)
#   make test270    run again with the settings for a 270 degree servo
#   make clean
#
# The simulation compiles the real sketch sources from ../../CycleTester together with the fake
# Arduino / servo / display / touch libraries in mocks/.

SKETCH ?= ../../CycleTester
CXX    ?= g++
# -std=gnu++11 is what the Arduino AVR compiler uses, so C++ that would not build on the Mega fails here too.
WARN     = -std=gnu++11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
CXXFLAGS = $(WARN) -Imocks -I$(SKETCH)

SRC = sim.cpp $(SKETCH)/CycleChannel.cpp $(SKETCH)/CycleChannel.h $(SKETCH)/Ui.cpp $(SKETCH)/Ui.h \
      $(SKETCH)/Config.h $(SKETCH)/CycleTester.ino $(SKETCH)/Font5x7.h $(wildcard mocks/*.h mocks/*.hpp)

.PHONY: all run pictures test270 clean
all: run

sim: $(SRC)
	$(CXX) $(CXXFLAGS) sim.cpp -o sim

run: sim
	@mkdir -p out
	./sim

pictures: run
	python3 render.py out

# Same simulation with a 270 degree servo: the 180 degree sweep must be centred in its travel (833..2167 us).
test270:
	rm -rf build/cfg270 && mkdir -p build/cfg270 out
	cp $(SKETCH)/* build/cfg270/
	sed -i 's/SERVO_FULL_TRAVEL_DEG = 180;/SERVO_FULL_TRAVEL_DEG = 270;/; s/SWEEP_START_OFFSET_DEG = 0;/SWEEP_START_OFFSET_DEG = 45;/' build/cfg270/Config.h
	grep -n "SERVO_FULL_TRAVEL_DEG =\|SWEEP_START_OFFSET_DEG =" build/cfg270/Config.h
	$(CXX) $(WARN) -Imocks -Ibuild/cfg270 sim.cpp -o build/sim270 && ./build/sim270 --short

clean:
	rm -rf sim out build
