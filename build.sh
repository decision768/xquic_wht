set -e
echo "Cleaning build directory..." 
rm -rf build 
echo "Creating build directory..." 
mkdir build 
cd build 
echo "Running CMake..." 
cmake -DCMAKE_BUILD_TYPE=Debug -DXQC_ENABLE_TESTING=1 -DXQC_SUPPORT_SENDMMSG_BUILD=1 -DXQC_ENABLE_EVENT_LOG=1 ..
echo "Building project..." 
make -j$(nproc)
echo "Build finished."
