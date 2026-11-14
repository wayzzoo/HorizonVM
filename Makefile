
CC = clang
CXX = clang++

TARGET = horizonvm

CFLAGS = -Wall -Wextra -Iinclude
CXXFLAGS = -std=c++20 -Wall -Wextra -Iinclude
LDFLAGS = -framework Hypervisor

C_SOURCES = \
    src/main.c \
    src/hypervisor/probe.c \
    src/hypervisor/loader_trace.c \
    src/hypervisor/vm.c \
    src/hypervisor/vcpu.c \
    src/hypervisor/memory.c \
    src/devices/timer.c

CPP_SOURCES = \
    src/platform/tegra210.cpp \
	src/devices/mmio.cpp

OBJECTS = $(patsubst %.c,build/%.o,$(C_SOURCES)) \
          $(patsubst %.cpp,build/%.o,$(CPP_SOURCES))

.PHONY: all run probe clean

all: $(TARGET)

$(TARGET): $(OBJECTS) HorizonVM.entitlements
	$(CXX) $(OBJECTS) -o $(TARGET) $(LDFLAGS)
	codesign --force --sign - --entitlements HorizonVM.entitlements ./$(TARGET)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: all
	./$(TARGET)

probe: all
	./$(TARGET) --probe

clean:
	rm -rf build $(TARGET)
