# Host-side simulation of the Cycle Tester sketch. No Arduino or hardware needed: just g++ and Python 3.
#
#   make            build and run the full simulation (takes a few seconds)
#   make pictures   also turn the saved screens in out/ into PNG files (needs Pillow)
#   make test180    run again with the settings for a 180 degree servo
#   make testnosave run again with saved calibration switched off in Config.h
#   make clean
#
# The simulation compiles the real sketch sources from ../../CycleTester together with the fake
# Arduino / servo / display / touch libraries in mocks/.

SKETCH ?= ../../CycleTester
CXX    ?= g++
# -std=gnu++11 is what the Arduino AVR compiler uses, so C++ that would not build on the Mega fails here too.
WARN     = -std=gnu++11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
CXXFLAGS = $(WARN) -Imocks -I$(SKETCH)

SRC = sim.cpp $(SKETCH)/CycleChannel.cpp $(SKETCH)/CycleChannel.h $(SKETCH)/Calibration.cpp $(SKETCH)/Calibration.h $(SKETCH)/Ui.cpp $(SKETCH)/Ui.h \
      $(SKETCH)/Config.h $(SKETCH)/CycleTester.ino $(SKETCH)/Font5x7.h $(wildcard mocks/*.h mocks/*.hpp)

.PHONY: all run pictures test180 testnosave clean
all: run

sim: $(SRC)
	$(CXX) $(CXXFLAGS) sim.cpp -o sim

run: sim
	@mkdir -p out
	./sim

pictures: run
	python3 render.py out

# Same simulation with a 180 degree servo (the factory default is 270): the sweep is the servo's whole travel, 500..2500 us.
test180:
	rm -rf build/cfg180 && mkdir -p build/cfg180 out
	cp $(SKETCH)/* build/cfg180/
	sed -i 's/SERVO_FULL_TRAVEL_DEG = 270;/SERVO_FULL_TRAVEL_DEG = 180;/; s/SWEEP_START_OFFSET_DEG = 45;/SWEEP_START_OFFSET_DEG = 0;/' build/cfg180/Config.h
	grep -n "SERVO_FULL_TRAVEL_DEG =\|SWEEP_START_OFFSET_DEG =" build/cfg180/Config.h
	$(CXX) $(WARN) -Imocks -Ibuild/cfg180 sim.cpp -o build/sim180 && ./build/sim180 --short

# Same simulation with USE_SAVED_CALIBRATION = false: SAVE must refuse and leave the stored values alone.
testnosave:
	rm -rf build/cfgnosave && mkdir -p build/cfgnosave out
	cp $(SKETCH)/* build/cfgnosave/
	sed -i 's/USE_SAVED_CALIBRATION = true;/USE_SAVED_CALIBRATION = false;/' build/cfgnosave/Config.h
	$(CXX) $(WARN) -Imocks -Ibuild/cfgnosave sim.cpp -o build/simnosave && ./build/simnosave --nosave

clean:
	rm -rf sim out build
