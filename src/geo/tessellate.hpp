#pragma once

#include <cstdint>
#include <vector>

namespace mapkern {

void triangulate(const std::vector<double>& coords, const std::vector<uint32_t>& holeStarts, std::vector<uint32_t>& out);
double triangleArea(const std::vector<double>& coords, const std::vector<uint32_t>& indices);

}
