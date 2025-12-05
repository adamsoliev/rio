CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
LDFLAGS = -luring

TARGET = rio
SRC = rio.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	sudo ./$(TARGET) --filename=/dev/nvme1n1 --type=randomread --size=1g --iodepth=32 --bs=4k

.PHONY: all clean run
