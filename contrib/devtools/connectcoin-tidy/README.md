# ConnectCoin Tidy

Example usage, starting from the repository root (this is a separate CMake project):

```bash
cd contrib/devtools/connectcoin-tidy

cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir) -DCMAKE_BUILD_TYPE=Release

cmake --build build -j$(nproc)

cmake --build build --target connectcoin-tidy-tests -j$(nproc)
```
