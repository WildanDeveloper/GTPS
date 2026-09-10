CXX      := g++
STD      := -std=c++17
WARN     := -Wall -Wextra -Wpedantic
OPT      := -O2

ROOT     := $(CURDIR)
BUILD    := $(ROOT)/build
ENET_SRC := thirdparty/enet
ENET_INC := $(ENET_SRC)/include

ENET_OBJECTS := $(patsubst $(ENET_SRC)/%.c,$(BUILD)/enet/%.o,$(wildcard $(ENET_SRC)/*.c))
GAME_SOURCES := $(wildcard src/*.cpp)
GAME_OBJECTS := $(patsubst src/%.cpp,$(BUILD)/src/%.o,$(GAME_SOURCES))

INCLUDES := -I$(ROOT)/src -I$(ENET_INC) -I/usr/include/mysql
LIBS     := $(BUILD)/libgtnet.a -lmysqlclient -lssl -lcrypto -lpthread

TARGET := $(BUILD)/WildanDev-game
TEST_TARGET := $(BUILD)/test-client
TEST_SOURCES := tests/client.cpp
TEST_OBJECTS := $(BUILD)/tests/client.o
UNIT_TARGET := $(BUILD)/test-units
UNIT_SOURCES := tests/units.cpp
UNIT_OBJECTS := $(BUILD)/tests/units.o $(BUILD)/src/items.o $(BUILD)/src/roles.o

all: $(TARGET)

$(BUILD)/enet/%.o: $(ENET_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -I$(ENET_INC) -c $< -o $@

$(BUILD)/libgtnet.a: $(ENET_OBJECTS)
	ar rcs $@ $^

$(BUILD)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(WARN) $(OPT) $(INCLUDES) -MMD -MP -c $< -o $@

-include $(GAME_OBJECTS:.o=.d)

$(TARGET): $(BUILD)/libgtnet.a $(GAME_OBJECTS)
	$(CXX) $(GAME_OBJECTS) -o $@ $(LIBS)

$(BUILD)/tests/client.o: tests/client.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(WARN) $(OPT) -I$(ENET_INC) -c $< -o $@

$(TEST_TARGET): $(BUILD)/libgtnet.a $(BUILD)/tests/client.o
	$(CXX) $(BUILD)/tests/client.o -o $@ $(BUILD)/libgtnet.a

$(BUILD)/tests/units.o: tests/units.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(WARN) $(OPT) -I$(ROOT)/src -I$(ROOT) -c $< -o $@

$(UNIT_TARGET): $(UNIT_OBJECTS)
	$(CXX) $(UNIT_OBJECTS) -o $@ -lssl -lcrypto

tests: $(TEST_TARGET) $(UNIT_TARGET)

clean:
	rm -rf $(BUILD)

.PHONY: all clean tests
