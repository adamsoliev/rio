CXX = g++
# CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
CXXFLAGS = -O3 -std=c++20 -fno-exceptions -Wall -pedantic -Werror -pthread -fno-omit-frame-pointer
LDFLAGS = -luring

TARGET = rio
SRC = rio.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	sudo ./$(TARGET) --filename=/dev/nvme1n1 --type=randomread --size=1g --iodepth=32 --bs=4k --mode=passthrough

.PHONY: all clean run
