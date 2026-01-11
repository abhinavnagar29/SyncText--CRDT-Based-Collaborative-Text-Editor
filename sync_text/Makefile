CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
LDFLAGS := -lrt

SRC := src/editor.cpp src/registry.cpp src/crdt.cpp
OBJ := $(SRC:.cpp=.o)
INC := -Iinclude

BIN := editor

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

clean:
	-pkill -9 editor 2>/dev/null || true
	rm -f $(OBJ) $(BIN)
	rm -f user_*_doc.txt
	rm -f /dev/shm/synctext_registry
	rm -f /dev/mqueue/queue_user_*
	rm -f *.log

.PHONY: all clean
