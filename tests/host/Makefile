# Host-side simulation of the Cycle Tester sketch. No Arduino or hardware needed: just g++ and Python 3.
#
#   make            build and run the full simulation (takes a few seconds)
#   make pictures   also turn the saved screens in out/ into PNG files (needs Pillow)
#   make test180    run again with the settings for a 180 degree servo
#   make testtrim   run again with non-zero degree trims / measured sweeps in Config.h
#   make testguards check that out-of-range Config.h values are refused at compile time
#   make clean
#
# The simulation compiles the real sketch sources from ../../CycleTester together with the fake
# Arduino / servo / display / touch libraries in mocks/.

SKETCH ?= ../../CycleTester
CXX    ?= g++
# -std=gnu++11 is what the Arduino AVR compiler uses, so C++ that would not build on the Mega fails here too.
WARN     = -std=gnu++11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
CXXFLAGS = $(WARN) -Imocks -I$(SKETCH)

SRC = sim.cpp $(SKETCH)/CycleChannel.cpp $(SKETCH)/CycleChannel.h $(SKETCH)/Ui.cpp $(SKETCH)/Ui.h $(SKETCH)/SerialCommands.cpp $(SKETCH)/SerialCommands.h \
      $(SKETCH)/Config.h $(SKETCH)/CycleTester.ino $(SKETCH)/Font5x7.h $(wildcard mocks/*.h mocks/*.hpp)

.PHONY: all run pictures test180 testtrim testguards clean
all: run

sim: $(SRC)
	$(CXX) $(CXXFLAGS) sim.cpp -o sim

run: sim
	@mkdir -p out
	./sim

pictures: run
	python3 render.py out

# Same simulation with a 180 degree servo (the factory default is 270): the sweep is the servo's whole travel, 500..2500 us.
# 2500 us is above the 2476 us the Servo library clips at unless Config.h raises its limit, so this also proves that fix.
test180:
	rm -rf build/cfg180 && mkdir -p build/cfg180 out
	cp $(SKETCH)/* build/cfg180/
	sed -i 's/SERVO_FULL_TRAVEL_DEG = 270;/SERVO_FULL_TRAVEL_DEG = 180;/; s/SWEEP_START_OFFSET_DEG = 45;/SWEEP_START_OFFSET_DEG = 0;/' build/cfg180/Config.h
	grep -n "SERVO_FULL_TRAVEL_DEG =\|SWEEP_START_OFFSET_DEG =" build/cfg180/Config.h
	$(CXX) $(WARN) -Imocks -Ibuild/cfg180 sim.cpp -o build/sim180 && ./build/sim180 --short

# Same simulation with fine-tuning numbers in Config.h; the pulse widths must come out as worked out by hand (see scenarioTrim).
testtrim:
	rm -rf build/cfgtrim && mkdir -p build/cfgtrim out
	cp $(SKETCH)/* build/cfgtrim/
	sed -i 's/\(SERVO_START_TRIM_DEG\[NUM_SERVOS\] *= *\){[^}]*}/\1{ 2.0f, -1.5f, 0.5f, 0.0f }/; s/\(SERVO_MEASURED_SWEEP_DEG\[NUM_SERVOS\] *= *\){[^}]*}/\1{ 180.0f, 180.0f, 177.5f, 183.0f }/' build/cfgtrim/Config.h
	grep -n "^constexpr float SERVO_" build/cfgtrim/Config.h
	$(CXX) $(WARN) -Imocks -Ibuild/cfgtrim sim.cpp -o build/simtrim && ./build/simtrim --short --trim

# Values that would drive a pulse outside the servo's range, or a nonsense measured sweep, must stop the compile.
testguards:
	./check_guards.sh $(SKETCH) "$(CXX)" "$(WARN)"

clean:
	rm -rf sim out build
