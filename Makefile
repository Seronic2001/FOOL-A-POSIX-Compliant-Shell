#Compiler
CXX = g++

#Compiler flags:  -g for debugging info
CXXFLAGS = -std=c++17 -g -Wall

#Executable name and Diretory
EXECUTABLE = FOOL
BUILD_DIR = build
SRC_DIR = src

#Source .cpp files (all live in src/)
SOURCES = $(addprefix $(SRC_DIR)/, main.cpp prompt.cpp parser.cpp executor.cpp builtins.cpp autocomplete.cpp line_editor.cpp history.cpp shell_signals.cpp)

#Generate object files from source files (src/foo.cpp -> build/foo.o)
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

#The default goal: build the executable
all: $(EXECUTABLE)

#Rule to link the executable from all the object files
$(EXECUTABLE): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(EXECUTABLE) $(OBJECTS)

#Rule to compile a .cpp file into a .o file inside the build directory
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(BUILD_DIR) 
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) -c $< -o $@

# Rule to clean up the build directory
clean:
	rm -rf $(BUILD_DIR)

# Rule to clean up everything (build directory and executables)
distclean: clean
	rm -f $(EXECUTABLE)

# ---------------------------------------------------------------- Testing

# Unit test binaries. Built from tests/test_*.cpp against the project
# sources; headers are found via -I.
UNIT_TESTS = $(BUILD_DIR)/test_parser $(BUILD_DIR)/test_builtins

$(BUILD_DIR)/test_%: tests/test_%.cpp $(OBJECTS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(filter-out $(BUILD_DIR)/main.o,$(OBJECTS)) -o $@

.PHONY: tests test-unit test-batch test-interactive test

tests: $(UNIT_TESTS)

test-unit: $(UNIT_TESTS)
	$(BUILD_DIR)/test_parser
	$(BUILD_DIR)/test_builtins

test-batch: $(EXECUTABLE)
	python3 tests/test_batch.py

test-interactive: $(EXECUTABLE)
	python3 tests/test_interactive.py

# Full suite: unit, batch, interactive.
test: test-unit test-batch test-interactive