BUILD_DIR = build
TARGET = Reality64

all: $(BUILD_DIR)
	cmake -B $(BUILD_DIR) -S .
	cmake --build $(BUILD_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
