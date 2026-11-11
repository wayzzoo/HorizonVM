CC = clang
TARGET = horizonvm

SOURCES = src/main.c \
          src/hypervisor/vm.c \
		  src/hypervisor/vcpu.c \
		  src/hypervisor/memory.c

CFLAGS = -Wall -Wextra -Iinclude
LDFLAGS = -framework Hypervisor

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SOURCES) HorizonVM.entitlements
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)
	codesign --force --sign - --entitlements HorizonVM.entitlements ./$(TARGET)

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)