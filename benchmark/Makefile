CXX = g++
CXXFLAGS = -O3 -std=c++20 -fno-exceptions -Wall -pedantic -Werror -pthread -fno-omit-frame-pointer
LDFLAGS = -luring

TARGET = benchmark
SRC = benchmark.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	sudo ./$(TARGET)

perf: $(TARGET)
	sudo perf stat -d ./$(TARGET)

perf-trace: $(TARGET)
	sudo perf stat -e "io_uring:*" ./$(TARGET)

.PHONY: all clean run
