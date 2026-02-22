CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
TARGET   = modbus_sniffer

all: $(TARGET)

$(TARGET): modbus_sniffer.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
