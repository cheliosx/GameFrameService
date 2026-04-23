mkdir -p build_release && cd build_release
cmake .. -DCMAKE_BUILD_TYPE=Release  # 关键参数：切换线上模式
make -j4