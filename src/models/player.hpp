#pragma once

#include <cstdint>
#include <string>

struct Player {
    std::uint64_t id = 0;
    int level = 1;
    std::string name;
    float x = 0.0F;
    float y = 0.0F;
};
