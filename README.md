# alpaca-pass

LLVM Pass for Alpaca programming model reference implementation

Build:

	$ mkdir build
	$ cd build
	$ cmake ..
	$ make

Run:

	$ clang -Xclang -load -Xclang build/src/libAlpacaPass.* something.c
