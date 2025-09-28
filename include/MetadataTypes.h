#pragma once

#include <string>
#include <vector>

namespace gymjot {

struct MetadataEntry {
    std::string key;
    std::string value;
};

using MetadataList = std::vector<MetadataEntry>;

}
