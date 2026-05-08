#Compiler
CXX = g++

#Compiler flags:  -g for debugging info
CXXFLAGS = -std=c++17 -g -Wall

#Executable name and Diretory
EXECUTABLE = FOOL
BUILD_DIR = build

#Source .cpp files
SOURCES = main.cpp prompt.cpp parser.cpp executor.cpp builtins.cpp autocomplete.cpp line_editor.cpp history.cpp shell_signals.cpp

#Generate object files from source files
OBJECTS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

#The default goal: build the executable
all: $(EXECUTABLE)

#Rule to link the executable from all the object files
$(EXECUTABLE): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(EXECUTABLE) $(OBJECTS)

#Rule to compile a .cpp file into a .o file inside the build directory
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(BUILD_DIR) 
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to clean up the build directory
clean:
	rm -rf $(BUILD_DIR)

# Rule to clean up everything (build directory and executables)
distclean: clean
	rm -f $(EXECUTABLE)