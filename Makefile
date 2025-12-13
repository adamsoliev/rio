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

threads: $(TARGET)
	@echo "=== submit mode (no SQPOLL) ==="
	@sudo bash -c './$(TARGET) --filename=/dev/nvme0n1 --type=randomread --size=10g --iodepth=128 --bs=4k --submit=submit & sleep 1; ps -eLo pid,tid,comm | grep $$!; kill $$!'
	@echo ""
	@echo "=== sqpoll mode ==="
	@sudo bash -c './$(TARGET) --filename=/dev/nvme0n1 --type=randomread --size=10g --iodepth=128 --bs=4k --submit=sqpoll & sleep 1; ps -eLo pid,tid,comm | grep $$!; kill $$!'

.PHONY: all clean run threads
