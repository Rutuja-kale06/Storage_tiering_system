.PHONY: all build clean run

all: build

build:
	@echo "Using CMake build system..."
	@if not exist "build" mkdir build
	cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . && cd ..

run: build
	@.\build\Release\storage_tiering.exe $(ARGS)

clean:
	@if exist "build" rmdir /s /q build
