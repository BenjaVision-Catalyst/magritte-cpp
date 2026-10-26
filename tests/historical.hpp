#pragma once
#include <nlohmann/json.hpp>
// Earlier GT fixtures projected derived label defaults to raw accessor keys.
// Fresh inference fixtures check actual defaults without this historical projection.
inline void omitHistoricalLabelDefaults(nlohmann::json &graph) {
    for (auto &node : graph["descriptions"])
        if (node.contains("accessor") && node["accessor"].value("readSelector", "") == "label")
            node["properties"].erase("default");
}
